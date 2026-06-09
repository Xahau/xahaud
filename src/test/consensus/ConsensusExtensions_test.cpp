//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/jtx.h>
#include <xrpld/app/consensus/ActiveValidatorView.h>
#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/consensus/ConsensusExtensionsTick.h>
#include <xrpld/consensus/ConsensusProposal.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/digest.h>
#include <cstring>
#include <deque>

namespace ripple {
namespace test {

namespace {

uint256
makeHash(char const* label)
{
    return sha512Half(Slice(label, std::strlen(label)));
}

NodeID
makeNode(std::uint8_t id)
{
    NodeID node;
    node.zero();
    node.data()[NodeID::size() - 1] = id;
    return node;
}

std::string
makeExportSigBlob(uint256 const& txHash, PublicKey const& publicKey)
{
    std::string blob;
    blob.append(reinterpret_cast<char const*>(txHash.data()), uint256::size());
    blob.append(
        reinterpret_cast<char const*>(publicKey.data()), publicKey.size());
    blob.push_back('\x30');
    return blob;
}

struct FakeTxSet
{
    using ID = uint256;

    uint256 hash;

    uint256
    id() const
    {
        return hash;
    }
};

class FakePeerPosition
{
public:
    using Proposal = ConsensusProposal<NodeID, uint256, ExtendedPosition>;

    FakePeerPosition(NodeID const& nodeId, ExtendedPosition const& position)
        : proposal_(
              uint256{},
              Proposal::seqJoin,
              position,
              NetClock::time_point{},
              NetClock::time_point{},
              nodeId)
    {
    }

    Proposal const&
    proposal() const
    {
        return proposal_;
    }

private:
    Proposal proposal_;
};

struct FakeExtensions
{
    enum class SidecarKind : uint8_t { commit, reveal, exportSig };

    beast::Journal j_{beast::Journal::getNullSink()};
    EstablishState estState_{EstablishState::ConvergingTx};
    std::chrono::steady_clock::time_point revealPhaseStart_{};
    std::chrono::steady_clock::time_point commitHashConflictStart_{};
    bool explicitFinalProposalSent_{false};
    bool entropySetPublished_{false};
    std::chrono::steady_clock::time_point entropyPublishStart_{};
    bool exportSigGateStarted_{false};
    std::chrono::steady_clock::time_point exportSigGateStart_{};
    bool exportSigConvergenceFailed_{false};
    bool rngOn{false};
    bool localExportSigs{true};
    bool consensusExportTxns{false};
    bool exportOn{true};
    bool entropyFailed{false};
    std::size_t exportQuorum{4};
    std::size_t commits{4};
    std::size_t reveals{4};
    bool commitQuorum{true};
    bool minimumReveals{true};
    bool anyReveals{true};
    bool sendExplicitFinal{false};
    uint256 exportHash{makeHash("local-export-sig-set")};
    uint256 commitHash{makeHash("local-commit-set")};
    uint256 entropyHash{makeHash("local-entropy-set")};
    std::deque<uint256> exportHashSequence;
    std::deque<uint256> commitHashSequence;
    std::deque<uint256> entropyHashSequence;
    std::optional<FakeTxSet> explicitFinalTxSet;
    std::vector<uint256> fetchedExportSets;
    std::vector<uint256> fetchedEntropySets;
    std::vector<uint256> fetchedCommitSets;
    int commitBuilds = 0;
    int exportBuilds = 0;
    int entropyBuilds = 0;
    int participantDiagnostics = 0;
    int selfSeeds = 0;

    bool
    rngEnabled() const
    {
        return rngOn;
    }

    bool
    exportEnabled() const
    {
        return exportOn;
    }

    std::size_t
    quorumThreshold() const
    {
        return exportQuorum;
    }

    std::size_t
    exportSigQuorumThreshold() const
    {
        return exportQuorum;
    }

    std::size_t
    pendingCommitCount() const
    {
        return rngOn ? commits : 0;
    }

    std::size_t
    pendingRevealCount() const
    {
        return rngOn ? reveals : 0;
    }

    std::size_t
    expectedProposerCount() const
    {
        return 0;
    }

    bool
    hasQuorumOfCommits() const
    {
        return rngOn && commitQuorum;
    }

    bool
    hasMinimumReveals() const
    {
        return rngOn && minimumReveals;
    }

    bool
    hasAnyReveals() const
    {
        return rngOn && anyReveals;
    }

    uint256
    buildCommitSet(LedgerIndex)
    {
        ++commitBuilds;
        if (!commitHashSequence.empty())
        {
            auto ret = commitHashSequence.front();
            commitHashSequence.pop_front();
            return ret;
        }
        return commitHash;
    }

    uint256
    buildEntropySet(LedgerIndex)
    {
        ++entropyBuilds;
        if (!entropyHashSequence.empty())
        {
            auto ret = entropyHashSequence.front();
            entropyHashSequence.pop_front();
            return ret;
        }
        return entropyHash;
    }

    uint256
    getEntropySecret() const
    {
        return makeHash("entropy-secret");
    }

    void
    selfSeedReveal()
    {
        ++selfSeeds;
    }

    void
    setEntropyFailed()
    {
        entropyFailed = true;
    }

    void
    fetchRngSetIfNeeded(std::optional<uint256> const& hash, SidecarKind kind)
    {
        if (kind == SidecarKind::reveal && hash)
            fetchedEntropySets.push_back(*hash);
        else if (kind == SidecarKind::commit && hash)
            fetchedCommitSets.push_back(*hash);
        else if (kind == SidecarKind::exportSig && hash)
            fetchedExportSets.push_back(*hash);
    }

    bool
    shouldSendExplicitFinalProposal() const
    {
        return sendExplicitFinal;
    }

    std::optional<FakeTxSet>
    buildExplicitFinalProposalTxSet(FakeTxSet const&, LedgerIndex)
    {
        return explicitFinalTxSet;
    }

    bool
    hasPendingExportSigs() const
    {
        return localExportSigs;
    }

    bool
    hasConsensusExportTxns() const
    {
        return consensusExportTxns;
    }

    uint256
    buildExportSigSet(LedgerIndex)
    {
        ++exportBuilds;
        if (!exportHashSequence.empty())
        {
            auto ret = exportHashSequence.front();
            exportHashSequence.pop_front();
            return ret;
        }
        return exportHash;
    }

    void
    setExportSigConvergenceFailed()
    {
        exportSigConvergenceFailed_ = true;
    }

    template <class PeerPositions>
    void
    recordParticipantDiagnostics(ConsensusMode, PeerPositions const&)
    {
        ++participantDiagnostics;
    }

    std::size_t
    observedParticipantCount() const
    {
        return 1;
    }

    std::optional<uint256>
    observedParticipantsHash() const
    {
        return makeHash("observed-participants");
    }

    std::string
    observedParticipantsBitmapBin() const
    {
        return "1";
    }
};

struct ExportTickHarness
{
    ExtendedPosition position{makeHash("tx-set")};
    FakeTxSet txns{position.txSetHash};
    hash_map<NodeID, FakePeerPosition> peers;
    ConsensusParms parms;
    NetClock::time_point netNow{NetClock::duration{123}};
    std::chrono::steady_clock::time_point start{};
    ConsensusMode mode = ConsensusMode::proposing;
    std::size_t prevProposers = 4;
    int updates = 0;
    int proposes = 0;
    int caches = 0;

    void
    addPeer(
        std::uint8_t id,
        std::optional<uint256> exportSigSetHash,
        uint256 txSetHash = makeHash("tx-set"))
    {
        ExtendedPosition peerPosition{txSetHash};
        peerPosition.exportSigSetHash = exportSigSetHash;
        peers.emplace(
            makeNode(id), FakePeerPosition{makeNode(id), peerPosition});
    }

    void
    addEntropyPeer(
        std::uint8_t id,
        std::optional<uint256> entropySetHash,
        uint256 txSetHash = makeHash("tx-set"))
    {
        ExtendedPosition peerPosition{txSetHash};
        peerPosition.entropySetHash = entropySetHash;
        peers.emplace(
            makeNode(id), FakePeerPosition{makeNode(id), peerPosition});
    }

    void
    addCommitPeer(
        std::uint8_t id,
        std::optional<uint256> commitSetHash,
        uint256 txSetHash = makeHash("tx-set"))
    {
        ExtendedPosition peerPosition{txSetHash};
        peerPosition.commitSetHash = commitSetHash;
        peers.emplace(
            makeNode(id), FakePeerPosition{makeNode(id), peerPosition});
    }

    ExtensionTickResult
    tick(FakeExtensions& ext, std::chrono::milliseconds elapsed = {})
    {
        ConsensusTick<ExtendedPosition, FakePeerPosition, FakeTxSet> ctx{
            .buildSeq = 2,
            .now = netNow,
            .nowSteady = start + elapsed,
            .roundTime = elapsed,
            .mode = mode,
            .prevProposers = prevProposers,
            .peerPositions = peers,
            .parms = parms,
            .haveCloseTimeConsensus = true,
            .convergePercent = 100,
            .j = beast::Journal{beast::Journal::getNullSink()},
            .getPosition = [&]() -> ExtendedPosition const& {
                return position;
            },
            .updatePosition =
                [&](ExtendedPosition const& newPosition) {
                    position = newPosition;
                    ++updates;
                },
            .propose = [&]() { ++proposes; },
            .haveConsensus = []() { return true; },
            .cacheAndShareTxSet = [&](FakeTxSet const&) { ++caches; },
            .getTxns = [&]() -> FakeTxSet const& { return txns; }};

        return extensionsTick(ext, ctx);
    }
};

}  // namespace

class ConsensusExtensions_test : public beast::unit_test::suite
{
    std::vector<PublicKey>
    makeValidatorKeys() const
    {
        std::vector<std::string> const rawKeys = {
            "0388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C05"
            "2",
            "02691AC5AE1C4C333AE5DF8A93BDC495F0EEBFC6DB0DA7EB6EF808F3AFC006E3F"
            "E"};

        std::vector<PublicKey> keys;
        keys.reserve(rawKeys.size());
        for (auto const& rawKey : rawKeys)
        {
            auto const pkHex = strUnHex(rawKey);
            keys.emplace_back(makeSlice(*pkHex));
        }
        return keys;
    }

    void
    testSidecarPeerAlignmentHelper()
    {
        testcase("Sidecar peer alignment helper");

        ExportTickHarness harness;
        auto const localHash = makeHash("sidecar-local");
        auto const conflictHash = makeHash("sidecar-conflict");
        harness.position.exportSigSetHash = localHash;
        harness.addPeer(1, localHash);
        harness.addPeer(2, conflictHash);
        harness.addPeer(3, std::nullopt);
        harness.addPeer(4, localHash, makeHash("other-tx-set"));

        std::vector<uint256> fetched;
        auto const state = detail::inspectTxConvergedSidecarPeers(
            harness.peers,
            harness.position,
            [](auto const& position) { return position.exportSigSetHash; },
            [&](auto const& hash) {
                if (hash)
                    fetched.push_back(*hash);
            });

        BEAST_EXPECT(state.localPublished);
        BEAST_EXPECT(state.conflict);
        BEAST_EXPECT(state.aligned == 1);
        BEAST_EXPECT(state.alignedParticipants() == 2);
        BEAST_EXPECT(state.peersSeen == 2);
        BEAST_EXPECT(state.txConverged == 3);
        BEAST_EXPECT(state.quorumAligned(2));
        BEAST_EXPECT(!state.quorumAligned(3));
        BEAST_EXPECT(!state.fullObservation());
        BEAST_EXPECT(fetched.size() == 1);
        if (!fetched.empty())
            BEAST_EXPECT(fetched.front() == conflictHash);

        harness.position.exportSigSetHash.reset();
        auto const unpublishedState = detail::inspectTxConvergedSidecarPeers(
            harness.peers,
            harness.position,
            [](auto const& position) { return position.exportSigSetHash; },
            [](auto const&) {});
        BEAST_EXPECT(!unpublishedState.localPublished);
        BEAST_EXPECT(unpublishedState.alignedParticipants() == 0);
        BEAST_EXPECT(!unpublishedState.quorumAligned(1));
        BEAST_EXPECT(unpublishedState.fullObservation());
    }

    void
    testActiveValidatorViewBuilderPrefersUNLReport()
    {
        testcase("Active validator view builder prefers UNLReport");

        auto const keys = makeValidatorKeys();
        ActiveValidatorViewSource source;
        source.sourceLedgerHash = makeHash("active-validator-view-source");
        source.unlReportMasterKeys.emplace();
        source.unlReportMasterKeys->insert(keys[0]);
        source.negativeUNLEnabled = true;
        source.negativeUNL.insert(keys[1]);

        ActiveValidatorViewFallback fallback;
        fallback.trustedMasterKeys.insert(keys[1]);
        fallback.localMasterKey = keys[1];

        auto const view = buildActiveValidatorView(source, fallback);
        BEAST_EXPECT(view.fromUNLReport);
        BEAST_EXPECT(view.sourceLedgerHash == source.sourceLedgerHash);
        BEAST_EXPECT(view.size() == 1);
        BEAST_EXPECT(view.containsMaster(keys[0]));
        BEAST_EXPECT(!view.containsMaster(keys[1]));
        BEAST_EXPECT(view.containsNode(calcNodeID(keys[0])));
    }

    void
    testActiveValidatorViewBuilderFallback()
    {
        testcase("Active validator view builder fallback");

        auto const keys = makeValidatorKeys();
        ActiveValidatorViewSource source;
        source.sourceLedgerHash = makeHash("active-validator-view-fallback");

        ActiveValidatorViewFallback fallback;
        fallback.trustedMasterKeys.insert(keys[0]);
        fallback.trustedMasterKeys.insert(keys[1]);
        fallback.localMasterKey = keys[1];

        auto const view = buildActiveValidatorView(source, fallback);
        BEAST_EXPECT(!view.fromUNLReport);
        BEAST_EXPECT(view.sourceLedgerHash == source.sourceLedgerHash);
        BEAST_EXPECT(view.size() == 2);
        BEAST_EXPECT(view.containsMaster(keys[0]));
        BEAST_EXPECT(view.containsMaster(keys[1]));

        source.negativeUNLEnabled = true;
        source.negativeUNL.insert(keys[0]);

        auto const negativeView = buildActiveValidatorView(source, fallback);
        BEAST_EXPECT(!negativeView.fromUNLReport);
        BEAST_EXPECT(negativeView.size() == 1);
        BEAST_EXPECT(!negativeView.containsMaster(keys[0]));
        BEAST_EXPECT(negativeView.containsMaster(keys[1]));
    }

    void
    testActiveValidatorViewAppliesNegativeUNL()
    {
        testcase("Active validator view applies NegativeUNL");

        using namespace jtx;
        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureNegativeUNL,
            nullptr};

        auto const vlKeys = makeValidatorKeys();
        auto const genesis = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());
        auto l = std::make_shared<Ledger>(
            *genesis, env.app().timeKeeper().closeTime());
        BEAST_EXPECT(l->rules().enabled(featureNegativeUNL));

        auto report = std::make_shared<SLE>(keylet::UNLReport());
        std::vector<STObject> activeValidators;
        for (auto const& pk : vlKeys)
        {
            activeValidators.push_back(
                STObject::makeInnerObject(sfActiveValidator));
            activeValidators.back().setFieldVL(sfPublicKey, pk);
        }
        report->setFieldArray(
            sfActiveValidators, STArray(activeValidators, sfActiveValidators));

        auto negUnl = std::make_shared<SLE>(keylet::negativeUNL());
        std::vector<STObject> disabledValidators;
        disabledValidators.push_back(
            STObject::makeInnerObject(sfDisabledValidator));
        disabledValidators.back().setFieldVL(sfPublicKey, vlKeys[0]);
        disabledValidators.back().setFieldU32(sfFirstLedgerSequence, l->seq());
        negUnl->setFieldArray(
            sfDisabledValidators,
            STArray(disabledValidators, sfDisabledValidators));
        OpenView accum(&*l);
        accum.rawInsert(report);
        accum.rawInsert(negUnl);
        accum.apply(*l);

        ConsensusExtensions ce{env.app(), env.journal};
        auto const view = ce.makeActiveValidatorView(l);

        BEAST_EXPECT(view->fromUNLReport);
        BEAST_EXPECT(view->size() == 1);
        BEAST_EXPECT(!view->containsMaster(vlKeys[0]));
        BEAST_EXPECT(!view->containsNode(calcNodeID(vlKeys[0])));
        BEAST_EXPECT(view->containsMaster(vlKeys[1]));
        BEAST_EXPECT(view->containsNode(calcNodeID(vlKeys[1])));
    }

    void
    testExportSigGateRequiresQuorumAlignment()
    {
        testcase("Export sig gate requires quorum alignment");

        FakeExtensions ext;
        ExportTickHarness harness;
        auto const localHash = ext.exportHash;

        harness.addPeer(1, localHash);
        harness.addPeer(2, localHash);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(harness.position.exportSigSetHash == localHash);
        BEAST_EXPECT(ext.exportSigGateStarted_);

        result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(!ext.exportSigConvergenceFailed_);

        result = harness.tick(
            ext,
            harness.parms.rngREVEAL_TIMEOUT * 2 + std::chrono::milliseconds{1});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.exportSigConvergenceFailed_);
    }

    void
    testRngEntropyGateRequiresFullObservation()
    {
        testcase("RNG entropy gate requires full sidecar observation");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingReveal;

        ExportTickHarness harness;
        auto const localHash = ext.entropyHash;

        harness.addEntropyPeer(1, localHash);
        harness.addEntropyPeer(2, localHash);
        harness.addEntropyPeer(3, localHash);
        harness.addEntropyPeer(4, std::nullopt);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(harness.position.entropySetHash == localHash);
        BEAST_EXPECT(ext.entropySetPublished_);

        // Quorum alignment is not safe if a tx-converged peer has not
        // advertised any entropySetHash. Otherwise local observation order
        // can split non-zero entropy from deterministic zero fallback.
        result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(!ext.entropyFailed);
        BEAST_EXPECT(harness.position.entropySetHash == localHash);

        result = harness.tick(
            ext,
            harness.parms.rngREVEAL_TIMEOUT * 2 + std::chrono::milliseconds{1});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.entropyFailed);
        BEAST_EXPECT(!harness.position.entropySetHash);
    }

    void
    testRngFastPathWaitsAfterEntropyPublish()
    {
        testcase("RNG fast path waits after entropy publish");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingCommit;

        ExportTickHarness harness;
        auto const localHash = ext.entropyHash;

        harness.addEntropyPeer(1, localHash);
        harness.addEntropyPeer(2, localHash);
        harness.addEntropyPeer(3, localHash);
        harness.addEntropyPeer(4, localHash);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingReveal);
        BEAST_EXPECT(ext.entropySetPublished_);
        BEAST_EXPECT(harness.position.entropySetHash == localHash);

        result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(!ext.entropyFailed);
        BEAST_EXPECT(harness.position.entropySetHash == localHash);
    }

    void
    testRngBootstrapSkipWhenPreviousParticipantsBelowQuorum()
    {
        testcase("RNG bootstrap skip below previous participant quorum");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;

        ExportTickHarness harness;
        harness.prevProposers = 2;

        auto result = harness.tick(ext);
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingTx);
        BEAST_EXPECT(ext.commitBuilds == 0);
        BEAST_EXPECT(!harness.position.commitSetHash);
    }

    void
    testRngCommitWaitsWhenQuorumPossible()
    {
        testcase("RNG commit waits when quorum is still possible");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.commitQuorum = false;
        ext.commits = 2;

        ExportTickHarness harness;
        harness.addCommitPeer(1, std::nullopt);
        harness.addCommitPeer(2, std::nullopt);
        harness.addCommitPeer(3, std::nullopt);

        auto result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingTx);
        BEAST_EXPECT(ext.commitBuilds == 0);
        BEAST_EXPECT(harness.updates == 0);
    }

    void
    testRngCommitTimeoutWithQuorumPublishesCommitSet()
    {
        testcase("RNG commit timeout with quorum publishes commit set");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.commitQuorum = false;
        ext.commits = 4;

        ExportTickHarness harness;
        harness.addCommitPeer(1, std::nullopt);
        harness.addCommitPeer(2, std::nullopt);
        harness.addCommitPeer(3, std::nullopt);

        auto result = harness.tick(
            ext,
            harness.parms.rngPIPELINE_TIMEOUT + std::chrono::milliseconds{1});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingCommit);
        BEAST_EXPECT(ext.commitBuilds == 1);
        BEAST_EXPECT(harness.position.commitSetHash == ext.commitHash);
        BEAST_EXPECT(harness.updates == 1);
        BEAST_EXPECT(harness.proposes == 1);
    }

    void
    testRngCommitQuorumInObservingModeDoesNotPropose()
    {
        testcase("RNG commit quorum in observing mode does not propose");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;

        ExportTickHarness harness;
        harness.mode = ConsensusMode::observing;

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingCommit);
        BEAST_EXPECT(harness.position.commitSetHash == ext.commitHash);
        BEAST_EXPECT(harness.updates == 1);
        BEAST_EXPECT(harness.proposes == 0);
        BEAST_EXPECT(ext.participantDiagnostics == 1);
    }

    void
    testRngCommitConflictRefreshesHashBeforeWaiting()
    {
        testcase("RNG commit conflict refreshes hash before waiting");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingCommit;
        auto const staleHash = makeHash("stale-commit-set");
        auto const refreshedHash = makeHash("refreshed-commit-set");
        auto const conflictHash = makeHash("conflicting-commit-set");
        ext.commitHashSequence.push_back(refreshedHash);

        ExportTickHarness harness;
        harness.start =
            std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
        harness.position.commitSetHash = staleHash;
        harness.addCommitPeer(1, conflictHash);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingCommit);
        BEAST_EXPECT(ext.commitBuilds == 1);
        BEAST_EXPECT(harness.position.commitSetHash == refreshedHash);
        BEAST_EXPECT(harness.updates == 1);
        BEAST_EXPECT(harness.proposes == 1);
        BEAST_EXPECT(ext.commitHashConflictStart_ == harness.start);
        BEAST_EXPECT(!ext.fetchedCommitSets.empty());
    }

    void
    testRngCommitHashConflictTimeoutFallsBack()
    {
        testcase("RNG commit hash conflict timeout falls back");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingCommit;
        ext.commitHash = makeHash("commit-local");

        ExportTickHarness harness;
        harness.start =
            std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
        harness.position.commitSetHash = ext.commitHash;
        auto const conflictHash = makeHash("commit-conflict");
        harness.addCommitPeer(1, conflictHash);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(!ext.entropyFailed);
        BEAST_EXPECT(ext.commitHashConflictStart_ == harness.start);
        BEAST_EXPECT(ext.fetchedCommitSets.size() == 1);
        BEAST_EXPECT(ext.fetchedCommitSets.front() == conflictHash);

        result = harness.tick(
            ext,
            harness.parms.rngREVEAL_TIMEOUT + std::chrono::milliseconds{1});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.entropyFailed);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingReveal);
        BEAST_EXPECT(
            ext.commitHashConflictStart_ ==
            std::chrono::steady_clock::time_point{});
    }

    void
    testRngRevealTransitionWaitsWhenRevealsIncomplete()
    {
        testcase("RNG reveal transition waits when reveals are incomplete");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingCommit;
        ext.minimumReveals = false;
        ext.reveals = 1;

        ExportTickHarness harness;
        harness.position.commitSetHash = ext.commitHash;

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingReveal);
        BEAST_EXPECT(ext.selfSeeds == 1);
        BEAST_EXPECT(ext.entropyBuilds == 0);
        BEAST_EXPECT(harness.position.myReveal);
        BEAST_EXPECT(!harness.position.entropySetHash);
        BEAST_EXPECT(harness.updates == 1);
        BEAST_EXPECT(harness.proposes == 1);
    }

    void
    testRngRevealTimeoutWithoutRevealsFallsBack()
    {
        testcase("RNG reveal timeout without reveals falls back");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingReveal;
        ext.minimumReveals = false;
        ext.anyReveals = false;

        ExportTickHarness harness;

        auto result = harness.tick(
            ext,
            harness.parms.rngREVEAL_TIMEOUT + std::chrono::milliseconds{1});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.entropyFailed);
        BEAST_EXPECT(!harness.position.entropySetHash);
    }

    void
    testRngEntropyPublishIsIdempotent()
    {
        testcase("RNG entropy publish is idempotent");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingReveal;

        ExportTickHarness harness;
        harness.position.entropySetHash = ext.entropyHash;

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.entropyBuilds == 1);
        BEAST_EXPECT(ext.entropySetPublished_);
        BEAST_EXPECT(harness.position.entropySetHash == ext.entropyHash);
        BEAST_EXPECT(harness.updates == 0);
        BEAST_EXPECT(harness.proposes == 0);
    }

    void
    testRngEntropyConflictTimeoutClearsHash()
    {
        testcase("RNG entropy conflict timeout clears hash");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingReveal;

        ExportTickHarness harness;
        harness.position.entropySetHash = ext.entropyHash;
        ext.entropySetPublished_ = true;
        ext.entropyPublishStart_ = harness.start;
        auto const conflictHash = makeHash("entropy-conflict");
        harness.addEntropyPeer(1, conflictHash);

        auto result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(!ext.entropyFailed);
        BEAST_EXPECT(harness.position.entropySetHash == ext.entropyHash);
        BEAST_EXPECT(!ext.fetchedEntropySets.empty());
        BEAST_EXPECT(ext.fetchedEntropySets.front() == conflictHash);

        result = harness.tick(
            ext,
            harness.parms.rngREVEAL_TIMEOUT * 2 + std::chrono::milliseconds{1});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.entropyFailed);
        BEAST_EXPECT(!harness.position.entropySetHash);
    }

    void
    testRngEntropyConflictRefreshesHashBeforeWaiting()
    {
        testcase("RNG entropy conflict refreshes hash before waiting");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingReveal;
        auto const staleHash = makeHash("stale-entropy-set");
        auto const refreshedHash = makeHash("refreshed-entropy-set");
        auto const conflictHash = makeHash("conflicting-entropy-set");
        ext.entropyHashSequence.push_back(staleHash);
        ext.entropyHashSequence.push_back(refreshedHash);

        ExportTickHarness harness;
        harness.start =
            std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
        harness.position.entropySetHash = staleHash;
        ext.entropySetPublished_ = true;
        ext.entropyPublishStart_ = harness.start;
        harness.addEntropyPeer(1, conflictHash);

        auto result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(!ext.entropyFailed);
        BEAST_EXPECT(ext.entropyBuilds == 2);
        BEAST_EXPECT(harness.position.entropySetHash == refreshedHash);
        BEAST_EXPECT(harness.updates == 1);
        BEAST_EXPECT(harness.proposes == 1);
        BEAST_EXPECT(!ext.fetchedEntropySets.empty());
    }

    void
    testRngEntropyConflictIgnoredWithQuorumAlignment()
    {
        testcase("RNG entropy conflict ignored with quorum alignment");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingReveal;

        ExportTickHarness harness;
        auto const localHash = ext.entropyHash;
        harness.position.entropySetHash = localHash;
        ext.entropySetPublished_ = true;
        ext.entropyPublishStart_ = harness.start;

        harness.addEntropyPeer(1, localHash);
        harness.addEntropyPeer(2, localHash);
        harness.addEntropyPeer(3, localHash);
        harness.addEntropyPeer(4, makeHash("entropy-minority-conflict"));

        auto result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(!ext.entropyFailed);
        BEAST_EXPECT(harness.position.entropySetHash == localHash);
        BEAST_EXPECT(ext.fetchedEntropySets.size() == 1);
    }

    void
    testRngExplicitFinalProposalPublishesSyntheticTxSet()
    {
        testcase("RNG explicit final proposal publishes synthetic tx set");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingReveal;
        ext.sendExplicitFinal = true;
        ext.explicitFinalTxSet = FakeTxSet{makeHash("explicit-final-tx-set")};

        ExportTickHarness harness;
        auto const localHash = ext.entropyHash;
        harness.position.entropySetHash = localHash;
        ext.entropySetPublished_ = true;
        ext.entropyPublishStart_ = harness.start;
        harness.addEntropyPeer(1, localHash);
        harness.addEntropyPeer(2, localHash);
        harness.addEntropyPeer(3, localHash);
        harness.addEntropyPeer(4, localHash);

        auto result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.explicitFinalProposalSent_);
        BEAST_EXPECT(
            harness.position.txSetHash == ext.explicitFinalTxSet->hash);
        BEAST_EXPECT(harness.position.entropySetHash == localHash);
        BEAST_EXPECT(harness.updates == 1);
        BEAST_EXPECT(harness.proposes == 1);
    }

    void
    testExportSigGateAllowsAlignedQuorumDespiteMinorityConflict()
    {
        testcase("Export sig gate ignores minority conflict after quorum");

        FakeExtensions ext;
        ExportTickHarness harness;
        auto const localHash = ext.exportHash;
        auto const conflictHash = makeHash("conflicting-export-sig-set");

        harness.addPeer(1, localHash);
        harness.addPeer(2, localHash);
        harness.addPeer(3, localHash);
        harness.addPeer(4, conflictHash);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);

        result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(!ext.exportSigConvergenceFailed_);
        BEAST_EXPECT(ext.fetchedExportSets.size() == 1);
        BEAST_EXPECT(ext.fetchedExportSets.front() == conflictHash);
    }

    void
    testExportSigGateRequiresFullObservation()
    {
        testcase("Export sig gate requires full sidecar observation");

        FakeExtensions ext;
        ExportTickHarness harness;
        auto const localHash = ext.exportHash;

        harness.addPeer(1, localHash);
        harness.addPeer(2, localHash);
        harness.addPeer(3, localHash);
        harness.addPeer(4, std::nullopt);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(harness.position.exportSigSetHash == localHash);
        BEAST_EXPECT(ext.exportSigGateStarted_);

        // Local quorum alignment is not enough if a tx-converged peer has
        // not advertised any exportSigSetHash yet.
        result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(!ext.exportSigConvergenceFailed_);

        result = harness.tick(
            ext,
            harness.parms.rngREVEAL_TIMEOUT * 2 + std::chrono::milliseconds{1});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.exportSigConvergenceFailed_);
    }

    void
    testExportSigGateFetchesAdvertisedPeerSets()
    {
        testcase("Export sig gate fetches advertised peer sets");

        FakeExtensions ext;
        ext.localExportSigs = false;
        ExportTickHarness harness;
        auto const peerHash = makeHash("peer-export-sig-set");

        harness.addPeer(1, peerHash);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.exportSigGateStarted_);
        BEAST_EXPECT(!harness.position.exportSigSetHash);
        BEAST_EXPECT(ext.fetchedExportSets.size() == 1);
        BEAST_EXPECT(ext.fetchedExportSets.front() == peerHash);

        result = harness.tick(
            ext,
            harness.parms.rngREVEAL_TIMEOUT * 2 + std::chrono::milliseconds{1});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.exportSigConvergenceFailed_);
    }

    void
    testExportSigGateObservingModeDoesNotPropose()
    {
        testcase("Export sig gate observing mode does not propose");

        FakeExtensions ext;
        ExportTickHarness harness;
        harness.mode = ConsensusMode::observing;

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(harness.position.exportSigSetHash == ext.exportHash);
        BEAST_EXPECT(harness.updates == 1);
        BEAST_EXPECT(harness.proposes == 0);
    }

    void
    testExportSigGateRefreshesHashBeforeWaiting()
    {
        testcase("Export sig gate refreshes hash before waiting");

        FakeExtensions ext;
        auto const staleHash = makeHash("stale-export-sig-set");
        auto const refreshedHash = makeHash("refreshed-export-sig-set");
        auto const conflictHash = makeHash("conflicting-export-sig-set");
        ext.exportHashSequence.push_back(staleHash);
        ext.exportHashSequence.push_back(refreshedHash);

        ExportTickHarness harness;
        harness.start =
            std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
        harness.position.exportSigSetHash = staleHash;
        ext.exportSigGateStarted_ = true;
        ext.exportSigGateStart_ = harness.start;
        harness.addPeer(1, conflictHash);

        auto result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(!ext.exportSigConvergenceFailed_);
        BEAST_EXPECT(ext.exportBuilds == 2);
        BEAST_EXPECT(harness.position.exportSigSetHash == refreshedHash);
        BEAST_EXPECT(harness.updates == 1);
        BEAST_EXPECT(harness.proposes == 1);
        BEAST_EXPECT(!ext.fetchedExportSets.empty());
    }

    void
    testExportSigGateBoundsCandidateObservationWindow()
    {
        testcase("Export sig gate bounds candidate observation window");

        FakeExtensions ext;
        ext.localExportSigs = false;
        ext.consensusExportTxns = true;
        ExportTickHarness harness;

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.exportSigGateStarted_);
        BEAST_EXPECT(!harness.position.exportSigSetHash);
        BEAST_EXPECT(ext.fetchedExportSets.empty());
        BEAST_EXPECT(!ext.exportSigConvergenceFailed_);

        result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(!ext.exportSigConvergenceFailed_);

        result = harness.tick(
            ext,
            harness.parms.rngREVEAL_TIMEOUT * 2 + std::chrono::milliseconds{1});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.exportSigConvergenceFailed_);
    }

    void
    testExportSigGateSkipsWhenExportDisabled()
    {
        testcase("Export sig gate skips when Export disabled");

        FakeExtensions ext;
        ext.exportOn = false;
        ExportTickHarness harness;

        harness.addPeer(1, ext.exportHash);

        auto result = harness.tick(ext);
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(!ext.exportSigGateStarted_);
        BEAST_EXPECT(!harness.position.exportSigSetHash);
        BEAST_EXPECT(ext.exportBuilds == 0);
        BEAST_EXPECT(ext.fetchedExportSets.empty());
    }

    void
    testParticipantDiagnosticsOnlyWhenExtensionEnabled()
    {
        testcase("Participant diagnostics only when extension enabled");

        FakeExtensions ext;
        ext.rngOn = false;
        ext.exportOn = false;
        ExportTickHarness harness;

        auto result = harness.tick(ext);
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.participantDiagnostics == 0);

        ext.exportOn = true;
        ext.localExportSigs = false;
        result = harness.tick(ext);
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.participantDiagnostics == 1);
    }

    void
    testExportDisabledRoundClearsCollector()
    {
        testcase("Export disabled round clears collector");

        using namespace jtx;
        Env env{*this, envconfig(), supported_amendments(), nullptr};
        ConsensusExtensions ce{env.app(), env.journal};
        auto const tx = makeHash("export-disabled-clears-collector");
        auto const pk = makeValidatorKeys().front();
        std::uint8_t const sigBytes[] = {1, 2, 3};
        Buffer const sig{sigBytes, sizeof(sigBytes)};

        ce.setExportEnabledThisRound(true);
        ce.exportSigCollector().addVerifiedSignature(tx, pk, sig, 10);
        ce.clearRngState();
        BEAST_EXPECT(ce.exportSigCollector().signatureCount(tx) == 1);

        ce.setExportEnabledThisRound(false);
        ce.clearRngState();
        BEAST_EXPECT(ce.exportSigCollector().signatureCount(tx) == 0);
    }

    void
    testReplayedProposalHarvestsExportSigs()
    {
        testcase("Replayed proposal harvests export signatures");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        ConsensusExtensions ce{env.app(), env.journal};
        ce.setExportEnabledThisRound(true);
        ce.cacheUNLReport();

        auto const activeView = ce.activeValidatorView();
        BEAST_EXPECT(activeView->sourceLedgerHash);
        if (!activeView->sourceLedgerHash)
            return;

        auto const senderPK = valKeys.keys->publicKey;
        BEAST_EXPECT(ce.isActiveValidator(senderPK, *activeView));
        if (!ce.isActiveValidator(senderPK, *activeView))
            return;

        auto const tx = makeHash("replayed-export-sig-tx");
        auto const blob = makeExportSigBlob(tx, senderPK);
        ExtendedPosition position{makeHash("replayed-position")};
        position.exportSignaturesHash =
            proposalExportSignaturesHash(std::vector<std::string>{blob});

        ce.onTrustedPeerProposal(
            calcNodeID(senderPK),
            senderPK,
            position,
            0,
            NetClock::time_point{},
            *activeView->sourceLedgerHash,
            Slice{},
            std::vector<std::string>{blob});

        BEAST_EXPECT(ce.exportSigCollector().hasUnverifiedSignatures());
    }

public:
    void
    run() override
    {
        testSidecarPeerAlignmentHelper();
        testActiveValidatorViewBuilderPrefersUNLReport();
        testActiveValidatorViewBuilderFallback();
        testActiveValidatorViewAppliesNegativeUNL();
        testExportSigGateRequiresQuorumAlignment();
        testRngEntropyGateRequiresFullObservation();
        testRngFastPathWaitsAfterEntropyPublish();
        testRngBootstrapSkipWhenPreviousParticipantsBelowQuorum();
        testRngCommitWaitsWhenQuorumPossible();
        testRngCommitTimeoutWithQuorumPublishesCommitSet();
        testRngCommitQuorumInObservingModeDoesNotPropose();
        testRngCommitConflictRefreshesHashBeforeWaiting();
        testRngCommitHashConflictTimeoutFallsBack();
        testRngRevealTransitionWaitsWhenRevealsIncomplete();
        testRngRevealTimeoutWithoutRevealsFallsBack();
        testRngEntropyPublishIsIdempotent();
        testRngEntropyConflictTimeoutClearsHash();
        testRngEntropyConflictRefreshesHashBeforeWaiting();
        testRngEntropyConflictIgnoredWithQuorumAlignment();
        testRngExplicitFinalProposalPublishesSyntheticTxSet();
        testExportSigGateAllowsAlignedQuorumDespiteMinorityConflict();
        testExportSigGateRequiresFullObservation();
        testExportSigGateFetchesAdvertisedPeerSets();
        testExportSigGateObservingModeDoesNotPropose();
        testExportSigGateRefreshesHashBeforeWaiting();
        testExportSigGateBoundsCandidateObservationWindow();
        testExportSigGateSkipsWhenExportDisabled();
        testParticipantDiagnosticsOnlyWhenExtensionEnabled();
        testExportDisabledRoundClearsCollector();
        testReplayedProposalHarvestsExportSigs();
    }
};

BEAST_DEFINE_TESTSUITE(ConsensusExtensions, consensus, ripple);

}  // namespace test
}  // namespace ripple
