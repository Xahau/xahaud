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
    bool localExportSigs{true};
    bool consensusExportTxns{false};
    bool exportOn{true};
    std::size_t exportQuorum{4};
    uint256 exportHash{makeHash("local-export-sig-set")};
    std::vector<uint256> fetchedExportSets;
    int exportBuilds = 0;

    bool
    rngEnabled() const
    {
        return false;
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
        return 0;
    }

    std::size_t
    pendingRevealCount() const
    {
        return 0;
    }

    std::size_t
    expectedProposerCount() const
    {
        return 0;
    }

    bool
    hasQuorumOfCommits() const
    {
        return false;
    }

    bool
    hasMinimumReveals() const
    {
        return false;
    }

    bool
    hasAnyReveals() const
    {
        return false;
    }

    uint256
    buildCommitSet(LedgerIndex)
    {
        return makeHash("commit-set");
    }

    uint256
    buildEntropySet(LedgerIndex)
    {
        return makeHash("entropy-set");
    }

    uint256
    getEntropySecret() const
    {
        return makeHash("entropy-secret");
    }

    void
    selfSeedReveal()
    {
    }

    void
    setEntropyFailed()
    {
    }

    void
    fetchRngSetIfNeeded(std::optional<uint256> const& hash, SidecarKind kind)
    {
        if (kind == SidecarKind::exportSig && hash)
            fetchedExportSets.push_back(*hash);
    }

    bool
    shouldSendExplicitFinalProposal() const
    {
        return false;
    }

    std::optional<FakeTxSet>
    buildExplicitFinalProposalTxSet(FakeTxSet const&, LedgerIndex)
    {
        return std::nullopt;
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
        return exportHash;
    }

    void
    setExportSigConvergenceFailed()
    {
        exportSigConvergenceFailed_ = true;
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
    int updates = 0;
    int proposes = 0;

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

    ExtensionTickResult
    tick(FakeExtensions& ext, std::chrono::milliseconds elapsed = {})
    {
        ConsensusTick<ExtendedPosition, FakePeerPosition, FakeTxSet> ctx{
            .buildSeq = 2,
            .now = netNow,
            .nowSteady = start + elapsed,
            .roundTime = elapsed,
            .mode = ConsensusMode::proposing,
            .prevProposers = 0,
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
            .cacheAndShareTxSet = [](FakeTxSet const&) {},
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
        testActiveValidatorViewAppliesNegativeUNL();
        testExportSigGateRequiresQuorumAlignment();
        testExportSigGateAllowsAlignedQuorumDespiteMinorityConflict();
        testExportSigGateFetchesAdvertisedPeerSets();
        testExportSigGateBoundsCandidateObservationWindow();
        testExportSigGateSkipsWhenExportDisabled();
        testExportDisabledRoundClearsCollector();
        testReplayedProposalHarvestsExportSigs();
    }
};

BEAST_DEFINE_TESTSUITE(ConsensusExtensions, consensus, ripple);

}  // namespace test
}  // namespace ripple
