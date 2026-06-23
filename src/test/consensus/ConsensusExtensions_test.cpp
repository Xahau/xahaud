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
#include <xrpld/app/ledger/InboundTransactions.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/misc/CanonicalTXSet.h>
#include <xrpld/app/misc/RuntimeConfig.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/consensus/ConsensusExtensionsTick.h>
#include <xrpld/consensus/ConsensusProposal.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/EntropyTier.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SidecarType.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>
#include <cstring>
#include <deque>

namespace ripple {
namespace test {

namespace {

class ActiveNoopSink : public beast::Journal::Sink
{
public:
    ActiveNoopSink() : Sink(beast::severities::kTrace, false)
    {
    }

    bool
    active(beast::severities::Severity) const override
    {
        return true;
    }

    void
    write(beast::severities::Severity, std::string const&) override
    {
    }

    void
    writeAlways(beast::severities::Severity, std::string const&) override
    {
    }
};

beast::Journal
activeNoopJournal()
{
    static ActiveNoopSink sink;
    return beast::Journal{sink};
}

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

STTx
makeSTTx(STObject const& obj)
{
    Serializer s;
    obj.add(s);
    SerialIter sit{s.slice()};
    return STTx{std::ref(sit)};
}

STObject
makeExportedPayment(AccountID const& src, AccountID const& dst)
{
    STObject obj(sfExportedTxn);
    obj.setFieldU16(sfTransactionType, ttPAYMENT);
    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);
    obj.setFieldU32(sfSequence, 0);
    obj.setFieldU32(sfTicketSequence, 1);
    obj.setFieldU32(sfFirstLedgerSequence, 2);
    obj.setFieldU32(sfLastLedgerSequence, 6);
    obj.setFieldAmount(sfAmount, XRPAmount{1000000});
    obj.setFieldAmount(sfFee, XRPAmount{10});
    obj.setFieldVL(sfSigningPubKey, Blob{});
    obj.setAccountID(sfAccount, src);
    obj.setAccountID(sfDestination, dst);
    return obj;
}

std::shared_ptr<STTx const>
makeExportTx(STObject const& inner, AccountID const& account)
{
    STObject exportObj(sfGeneric);
    exportObj.setFieldU16(sfTransactionType, ttEXPORT);
    exportObj.setAccountID(sfAccount, account);
    exportObj.setFieldU32(sfSequence, 0);
    exportObj.setFieldVL(sfSigningPubKey, Blob{});
    exportObj.setFieldU32(sfFirstLedgerSequence, 2);
    exportObj.setFieldU32(sfLastLedgerSequence, 6);
    exportObj.setFieldAmount(sfFee, XRPAmount{0});
    exportObj.set(std::make_unique<STObject>(inner));

    return std::make_shared<STTx const>(makeSTTx(exportObj));
}

RCLTxSet
makeRCLTxSet(Application& app, std::vector<std::shared_ptr<STTx const>> txns)
{
    auto map =
        std::make_shared<SHAMap>(SHAMapType::TRANSACTION, app.getNodeFamily());
    map->setUnbacked();

    for (auto const& tx : txns)
    {
        Serializer s;
        tx->add(s);
        map->addItem(
            SHAMapNodeType::tnTRANSACTION_NM,
            make_shamapitem(tx->getTransactionID(), s.slice()));
    }

    return RCLTxSet{map->snapShot(false)};
}

AccountID
accountFromNode(NodeID const& nodeId)
{
    AccountID acctId;
    std::memcpy(acctId.data(), nodeId.data(), acctId.size());
    return acctId;
}

std::shared_ptr<SHAMap>
makeSidecarSet(Application& app, std::vector<STObject> const& sidecars)
{
    auto map =
        std::make_shared<SHAMap>(SHAMapType::SIDECAR, app.getNodeFamily());
    map->setUnbacked();

    for (auto const& sidecar : sidecars)
    {
        auto const itemKey = sidecar.getHash(HashPrefix::sidecar);
        Serializer s(2048);
        sidecar.add(s);
        map->addItem(
            SHAMapNodeType::tnSIDECAR, make_shamapitem(itemKey, s.slice()));
    }

    return map->snapShot(false);
}

std::shared_ptr<SHAMap>
makeRawSidecarSet(Application& app, std::string const& raw)
{
    auto map =
        std::make_shared<SHAMap>(SHAMapType::SIDECAR, app.getNodeFamily());
    map->setUnbacked();
    map->addItem(
        SHAMapNodeType::tnSIDECAR,
        make_shamapitem(makeHash("raw-sidecar-entry"), makeSlice(raw)));
    return map->snapShot(false);
}

void
publishAndFetchSidecarSet(
    Application& app,
    ConsensusExtensions& ce,
    std::shared_ptr<SHAMap> const& map,
    ConsensusExtensions::SidecarKind kind)
{
    auto const hash = map->getHash().as_uint256();
    app.getInboundTransactions().giveSet(hash, map, false);
    ce.fetchRngSetIfNeeded(hash, kind);
}

void
forceNonStandalone(Application& app)
{
    const_cast<Config&>(app.config()).setupControl(true, true, false);
}

std::shared_ptr<STTx const>
singleCanonicalTx(CanonicalTXSet const& txs)
{
    if (std::distance(txs.begin(), txs.end()) != 1)
        return {};
    return txs.begin()->second;
}

uint256
expectedEntropy(PublicKey const& key, uint256 const& reveal)
{
    Serializer s;
    s.addVL(key.slice());
    s.addBitString(reveal);
    return sha512Half(s.slice());
}

Buffer
signPosition(
    PublicKey const& publicKey,
    SecretKey const& secretKey,
    ExtendedPosition const& position,
    std::uint32_t proposeSeq,
    NetClock::time_point closeTime,
    uint256 const& prevLedger)
{
    using Proposal = ConsensusProposal<NodeID, uint256, ExtendedPosition>;
    Proposal proposal{
        prevLedger,
        proposeSeq,
        position,
        closeTime,
        NetClock::time_point{},
        calcNodeID(publicKey)};

    auto const sig = signDigest(publicKey, secretKey, proposal.signingHash());
    return Buffer(sig.data(), sig.size());
}

Blob
makeProofBlob(
    PublicKey const& publicKey,
    SecretKey const& secretKey,
    ExtendedPosition const& position,
    std::uint32_t proposeSeq,
    NetClock::time_point closeTime,
    uint256 const& prevLedger)
{
    auto signature = signPosition(
        publicKey, secretKey, position, proposeSeq, closeTime, prevLedger);
    Serializer positionData;
    position.add(positionData);
    return ConsensusExtensions::serializeProof(
        ConsensusExtensions::ProposalProof{
            proposeSeq,
            static_cast<std::uint32_t>(closeTime.time_since_epoch().count()),
            prevLedger,
            std::move(positionData),
            std::move(signature)});
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

    beast::Journal j_{activeNoopJournal()};
    EstablishState estState_{EstablishState::ConvergingTx};
    std::chrono::steady_clock::time_point revealPhaseStart_{};
    std::chrono::steady_clock::time_point commitHashConflictStart_{};
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
    uint256 exportHash{makeHash("local-export-sig-set")};
    uint256 commitHash{makeHash("local-commit-set")};
    uint256 entropyHash{makeHash("local-entropy-set")};
    std::deque<uint256> exportHashSequence;
    std::deque<uint256> commitHashSequence;
    std::deque<uint256> entropyHashSequence;
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
    entropyGateThreshold() const
    {
        // Stub default mirrors quorumThreshold (tier-2 band collapsed); the
        // tier-2 step-down is exercised end-to-end in the CSF sims.
        return exportQuorum;
    }

    std::size_t
    exportSigQuorumThreshold() const
    {
        return exportQuorum;
    }

    // Membership is a no-op in the FakeExtensions tick tests (every peer
    // counts, local counts) so the gate behavior is unchanged; F1 active-view
    // filtering is exercised directly against inspectTxConvergedSidecarPeers.
    template <class Id>
    bool
    isUNLReportMember(Id const&) const
    {
        return true;
    }

    bool
    localIsActiveValidator() const
    {
        return true;
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
            .j = activeNoopJournal(),
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
            .getTxns = [&]() -> FakeTxSet const& { return txns; }};

        return extensionsTick(ext, ctx);
    }
};

// Build an in-memory ledger carrying a UNLReport with the given active
// validator keys (optionally disabling some via NegativeUNL). Drives the RNG
// active-validator view. NOTE: it is not registered with the LedgerMaster, so
// it suits cacheUNLReport()/makeActiveValidatorView() (which read its SLEs
// directly) but NOT reveal verification (which resolves the round's prev ledger
// through the LedgerMaster — anchor harvests to a real closed ledger instead).
std::shared_ptr<Ledger>
makeUNLReportLedger(
    jtx::Env& env,
    std::vector<PublicKey> const& activeKeys,
    std::vector<PublicKey> const& disabledKeys = {})
{
    auto const genesis = std::make_shared<Ledger>(
        create_genesis,
        env.app().config(),
        std::vector<uint256>{},
        env.app().getNodeFamily());
    auto ledger =
        std::make_shared<Ledger>(*genesis, env.app().timeKeeper().closeTime());

    auto report = std::make_shared<SLE>(keylet::UNLReport());
    std::vector<STObject> active;
    active.reserve(activeKeys.size());
    for (auto const& pk : activeKeys)
    {
        active.push_back(STObject::makeInnerObject(sfActiveValidator));
        active.back().setFieldVL(sfPublicKey, pk);
    }
    report->setFieldArray(
        sfActiveValidators, STArray(active, sfActiveValidators));

    OpenView accum(&*ledger);
    accum.rawInsert(report);

    if (!disabledKeys.empty())
    {
        auto negUnl = std::make_shared<SLE>(keylet::negativeUNL());
        std::vector<STObject> disabled;
        disabled.reserve(disabledKeys.size());
        for (auto const& pk : disabledKeys)
        {
            disabled.push_back(STObject::makeInnerObject(sfDisabledValidator));
            disabled.back().setFieldVL(sfPublicKey, pk);
            disabled.back().setFieldU32(sfFirstLedgerSequence, ledger->seq());
        }
        negUnl->setFieldArray(
            sfDisabledValidators, STArray(disabled, sfDisabledValidators));
        accum.rawInsert(negUnl);
    }

    accum.apply(*ledger);
    return ledger;
}

// Harvest a commit (proposeSeq 0) + matching reveal (proposeSeq 1) from one
// validator into `ce`. The commitment binds seq, which reveal verification
// recomputes as prevLedger->seq()+1 (resolved through the LedgerMaster), so
// pass a prevLedger that is actually stored and a matching seq.
void
harvestCommitReveal(
    ConsensusExtensions& ce,
    NodeID const& nodeId,
    PublicKey const& pk,
    SecretKey const& sk,
    uint256 const& txSetHash,
    LedgerIndex seq,
    NetClock::time_point closeTime,
    uint256 const& prevLedger,
    uint256 const& reveal)
{
    // nodeId is explicit (not calcNodeID(pk)): a validator's view identity is
    // its master-key NodeID, which can differ from its signing pubkey.
    auto const commitment = sha512Half(reveal, pk, seq);

    ExtendedPosition commitPos{txSetHash};
    commitPos.myCommitment = commitment;
    auto const commitSig =
        signPosition(pk, sk, commitPos, 0, closeTime, prevLedger);
    ce.harvestRngData(
        nodeId,
        pk,
        commitPos,
        0,
        closeTime,
        prevLedger,
        Slice(commitSig.data(), commitSig.size()));

    ExtendedPosition revealPos{txSetHash};
    revealPos.myReveal = reveal;
    auto const revealSig =
        signPosition(pk, sk, revealPos, 1, closeTime, prevLedger);
    ce.harvestRngData(
        nodeId,
        pk,
        revealPos,
        1,
        closeTime,
        prevLedger,
        Slice(revealSig.data(), revealSig.size()));
}

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

        auto const exportHashOf = [](auto const& position) {
            return position.exportSigSetHash;
        };
        auto const allMembers = [](auto const&) { return true; };

        std::vector<uint256> fetched;
        auto const state = detail::inspectTxConvergedSidecarPeers(
            harness.peers,
            harness.position,
            true,
            exportHashOf,
            allMembers,
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
            true,
            exportHashOf,
            allMembers,
            [](auto const&) {});
        BEAST_EXPECT(!unpublishedState.localPublished);
        BEAST_EXPECT(unpublishedState.alignedParticipants() == 0);
        BEAST_EXPECT(!unpublishedState.quorumAligned(1));
        BEAST_EXPECT(unpublishedState.fullObservation());

        //@@start test-sidecar-active-view-filter
        // F1: the alignment-counting universe must be the active validator
        // view, not the full trusted-proposer set. A trusted-but-non-active
        // proposer (node 5) that tx-converges and aligns on the SAME hash must
        // NOT inflate alignedParticipants() — otherwise two equivocation
        // cohorts padded by non-active peers could each clear the gate.
        harness.position.exportSigSetHash = localHash;
        harness.addPeer(5, localHash);  // trusted, but outside the active view
        auto const activeOnly = [](auto const& id) {
            return id != makeNode(5);  // nodes 1..4 active; 5 is not
        };

        auto const padded = detail::inspectTxConvergedSidecarPeers(
            harness.peers,
            harness.position,
            true,
            exportHashOf,
            allMembers,
            [](auto const&) {});
        BEAST_EXPECT(padded.aligned == 2);  // node 1 + node 5, unfiltered

        auto const filtered = detail::inspectTxConvergedSidecarPeers(
            harness.peers,
            harness.position,
            true,
            exportHashOf,
            activeOnly,
            [](auto const&) {});
        BEAST_EXPECT(filtered.aligned == 1);                // node 5 excluded
        BEAST_EXPECT(filtered.alignedParticipants() == 2);  // node 1 + local
        BEAST_EXPECT(!filtered.quorumAligned(3));  // padding can't reach quorum

        // The local +1 is likewise gated on local active-view membership.
        auto const nonActiveLocal = detail::inspectTxConvergedSidecarPeers(
            harness.peers,
            harness.position,
            false,
            exportHashOf,
            activeOnly,
            [](auto const&) {});
        BEAST_EXPECT(!nonActiveLocal.localPublished);
        BEAST_EXPECT(nonActiveLocal.alignedParticipants() == 1);  // node 1 only
        //@@end test-sidecar-active-view-filter
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
        BEAST_EXPECT(view.originalViewSize == 1);
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
        BEAST_EXPECT(view.originalViewSize == 2);
        BEAST_EXPECT(view.containsMaster(keys[0]));
        BEAST_EXPECT(view.containsMaster(keys[1]));

        source.negativeUNLEnabled = true;
        source.negativeUNL.insert(keys[0]);

        auto const negativeView = buildActiveValidatorView(source, fallback);
        BEAST_EXPECT(!negativeView.fromUNLReport);
        BEAST_EXPECT(negativeView.size() == 1);
        // nUNL shrinks the effective view but NOT the original-UNL denominator.
        BEAST_EXPECT(negativeView.originalViewSize == 2);
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
        auto const l = makeUNLReportLedger(env, vlKeys, {vlKeys[0]});
        BEAST_EXPECT(l->rules().enabled(featureNegativeUNL));

        ConsensusExtensions ce{env.app(), env.journal};
        auto const view = ce.makeActiveValidatorView(l);

        BEAST_EXPECT(view->fromUNLReport);
        BEAST_EXPECT(view->size() == 1);
        // Effective view is 1 (nUNL disabled one), but the original-UNL
        // denominator that Tier 2's intersection floor anchors to stays 2.
        BEAST_EXPECT(view->originalViewSize == 2);
        BEAST_EXPECT(!view->containsMaster(vlKeys[0]));
        BEAST_EXPECT(!view->containsNode(calcNodeID(vlKeys[0])));
        BEAST_EXPECT(view->containsMaster(vlKeys[1]));
        BEAST_EXPECT(view->containsNode(calcNodeID(vlKeys[1])));
    }

    void
    testActiveValidatorViewNullSourceAndExpectedProposers()
    {
        testcase("Active validator view null source and expected proposers");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        auto const view = ce.makeActiveValidatorView({});
        BEAST_EXPECT(!view->fromUNLReport);
        BEAST_EXPECT(view->containsNode(valKeys.nodeID));

        ce.cacheUNLReport();
        ce.setMode(ConsensusMode::proposing);
        ce.setExpectedProposers(hash_set<NodeID>{valKeys.nodeID, makeNode(99)});
        BEAST_EXPECT(ce.expectedProposerCount() == 1);

        ce.setExpectedProposers({});
        BEAST_EXPECT(
            ce.expectedProposerCount() == ce.activeValidatorView()->size());
    }

    void
    testParticipantThreshold()
    {
        testcase("participant-alignment threshold arithmetic");

        // Smallest cohort t with 2t - n > floor(n/5) tolerated faults: ~0.6 n,
        // but bumped at multiples of 5 where plain ceil(0.6 n) leaves the
        // cohort overlap exactly == f (forkable).
        BEAST_EXPECT(calculateParticipantThreshold(1) == 1);
        BEAST_EXPECT(
            calculateParticipantThreshold(5) == 4);  // not 3 (n=5 forks)
        BEAST_EXPECT(calculateParticipantThreshold(6) == 4);  // == ceil(0.6*6)
        BEAST_EXPECT(calculateParticipantThreshold(7) == 5);
        BEAST_EXPECT(calculateParticipantThreshold(8) == 5);
        BEAST_EXPECT(
            calculateParticipantThreshold(10) == 7);  // not 6 (n=10 forks)
        BEAST_EXPECT(calculateParticipantThreshold(15) == 10);
        BEAST_EXPECT(calculateParticipantThreshold(20) == 13);

        // The defining safety invariant, for EVERY view size: two aligned
        // cohorts always share an honest validator, i.e. their overlap (2t - n)
        // strictly exceeds the tolerated Byzantine count f = floor(n/5). And
        // the Tier 2 bar is never stricter than the Tier 3 validator_quorum
        // bar.
        for (std::size_t n = 1; n <= 256; ++n)
        {
            auto const t = calculateParticipantThreshold(n);
            BEAST_EXPECT(2 * t > n + n / 5);  // overlap 2t-n > floor(n/5)
            BEAST_EXPECT(t <= calculateQuorumThreshold(n));
        }
    }

    void
    testRuntimeConfigPolicyAccessors()
    {
        testcase("runtime config policy accessors");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        ConsensusExtensions ce{env.app(), activeNoopJournal()};

        BEAST_EXPECT(!ce.bootstrapFastStartEnabled());

        ConsensusTestConfig cfg;
        cfg.bootstrapFastStart = true;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);
        BEAST_EXPECT(ce.bootstrapFastStartEnabled());

        cfg.bootstrapFastStart = false;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);
        BEAST_EXPECT(!ce.bootstrapFastStartEnabled());
    }

    void
    testDecoratePositionGeneratesCommitment()
    {
        testcase("decoratePosition generates commitment");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy,
            nullptr};
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ExtendedPosition pos{makeHash("decorate-position-enabled")};
        ce.decoratePosition(pos, ledger, true);

        BEAST_EXPECT(pos.myCommitment);
        BEAST_EXPECT(ce.pendingCommitCount() == 1);
        BEAST_EXPECT(ce.getEntropySecret() != uint256{});
        BEAST_EXPECT(ce.isUNLReportMember(env.app().getValidatorKeys().nodeID));
    }

    void
    testOnPreBuildInjectsZeroEntropyFallback()
    {
        testcase("onPreBuild injects consensus-bound fallback entropy");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy,
            nullptr};
        forceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        ce.onRoundStart(RCLCxLedger{ledger}, {});
        ce.setRngEnabledThisRound(true);

        CanonicalTXSet retriableTxs{makeHash("rng-zero-fallback-salt")};
        auto const seq = ledger->seq() + 1;
        auto const txSetHash = makeHash("rng-fallback-txset");
        ce.onPreBuild(retriableTxs, seq, txSetHash);

        auto const tx = singleCanonicalTx(retriableTxs);
        BEAST_EXPECT(tx);
        if (!tx)
            return;
        BEAST_EXPECT(tx->getTxnType() == ttCONSENSUS_ENTROPY);
        BEAST_EXPECT(tx->getFieldU32(sfLedgerSequence) == seq);

        // Tier 1: deterministic, consensus-bound, non-zero, fallback-labeled.
        auto const expected = sha512Half(
            HashPrefix::entropyFallback, ledger->info().hash, txSetHash, seq);
        BEAST_EXPECT(tx->getFieldH256(sfDigest) == expected);
        BEAST_EXPECT(tx->getFieldH256(sfDigest) != uint256{});
        BEAST_EXPECT(tx->getFieldU16(sfEntropyCount) == 0);
        BEAST_EXPECT(
            tx->getFieldU8(sfEntropyTier) == entropyTierConsensusFallback);

        // Same agreed inputs => identical digest on an independent instance.
        ConsensusExtensions other{env.app(), activeNoopJournal()};
        other.onRoundStart(RCLCxLedger{ledger}, {});
        other.setRngEnabledThisRound(true);
        CanonicalTXSet otherTxs{makeHash("rng-zero-fallback-salt-2")};
        other.onPreBuild(otherTxs, seq, txSetHash);
        auto const otherTx = singleCanonicalTx(otherTxs);
        BEAST_EXPECT(otherTx);
        if (otherTx)
            BEAST_EXPECT(otherTx->getFieldH256(sfDigest) == expected);
    }

    void
    testOnPreBuildInjectsEntropySetEntropy()
    {
        testcase("onPreBuild injects entropy-set entropy");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy,
            nullptr};
        forceNonStandalone(env.app());
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        auto const& publicKey = valKeys.keys->publicKey;
        auto const& secretKey = valKeys.keys->secretKey;
        auto const nodeId = valKeys.nodeID;
        auto const prevLedger = ledger->info().hash;
        auto const seq = ledger->seq() + 1;
        auto const closeTime = NetClock::time_point{NetClock::duration{321}};
        auto const txSetHash = makeHash("prebuild-entropy-txset");
        auto const reveal = makeHash("prebuild-entropy-reveal");
        auto const viewLedger =
            makeUNLReportLedger(env, std::vector<PublicKey>{publicKey});
        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.cacheUNLReport(viewLedger);
        harvestCommitReveal(
            ce,
            nodeId,
            publicKey,
            secretKey,
            txSetHash,
            seq,
            closeTime,
            prevLedger,
            reveal);
        BEAST_EXPECT(ce.hasQuorumOfCommits());
        BEAST_EXPECT(ce.hasMinimumReveals());

        auto const entropySetHash = ce.buildEntropySet(seq);
        BEAST_EXPECT(ce.isSidecarSet(entropySetHash));

        CanonicalTXSet retriableTxs{makeHash("entropy-set-prebuild-salt")};
        ce.onPreBuild(retriableTxs, seq, txSetHash);

        auto const tx = singleCanonicalTx(retriableTxs);
        BEAST_EXPECT(tx);
        if (!tx)
            return;
        BEAST_EXPECT(tx->getTxnType() == ttCONSENSUS_ENTROPY);
        BEAST_EXPECT(
            tx->getFieldH256(sfDigest) == expectedEntropy(publicKey, reveal));
        BEAST_EXPECT(tx->getFieldU16(sfEntropyCount) == 1);
        BEAST_EXPECT(
            tx->getFieldU8(sfEntropyTier) == entropyTierValidatorQuorum);
    }

    void
    testOnPreBuildTier2ParticipantAligned()
    {
        testcase(
            "onPreBuild labels sub-quorum aligned set participant_aligned");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy,
            nullptr};
        forceNonStandalone(env.app());

        // A 6-validator UNLReport opens the tier-2 band:
        //   tier2Threshold  = 4 (smallest cohort whose overlap exceeds f=1)
        //   quorumThreshold = ceil(0.8 * 6) = 5
        // so an agreed aligned count of 4 is participant_aligned, 5+ is
        // validator_quorum, and < 4 falls back. (n=5 has no band -- there
        // tier2 == quorum -- so 6 is the smallest size that exercises tier 2.)
        constexpr std::size_t kValidators = 6;
        std::vector<std::pair<PublicKey, SecretKey>> vals;
        vals.reserve(kValidators);
        std::vector<PublicKey> activeKeys;
        activeKeys.reserve(kValidators);
        for (std::size_t i = 0; i < kValidators; ++i)
        {
            vals.push_back(randomKeyPair(KeyType::secp256k1));
            activeKeys.push_back(vals.back().first);
        }
        auto const viewLedger = makeUNLReportLedger(env, activeKeys);

        // Thresholds derive from the cached pre-nUNL view (originalViewSize=6).
        {
            ConsensusExtensions ce{env.app(), activeNoopJournal()};
            ce.cacheUNLReport(viewLedger);
            BEAST_EXPECT(ce.activeValidatorView()->originalViewSize == 6);
            BEAST_EXPECT(ce.quorumThreshold() == 5);
            BEAST_EXPECT(ce.tier2Threshold() == 4);
            BEAST_EXPECT(ce.entropyGateThreshold() == 4);
        }

        // Reveal verification resolves the round's prev ledger through the
        // LedgerMaster, so anchor commitments to a real closed ledger (the view
        // above comes from the in-memory UNLReport ledger).
        auto const anchor = env.app().getLedgerMaster().getClosedLedger();
        auto const prevLedger = anchor->info().hash;
        auto const seq = anchor->info().seq + 1;
        auto const closeTime = NetClock::time_point{NetClock::duration{777}};
        auto const txSetHash = makeHash("tier2-txset");

        // Harvest commit+reveal from `revealers` of the 6 validators, build the
        // agreed entropy set, inject, and return the labelled (tier, count).
        auto runWith = [&](std::size_t revealers) {
            ConsensusExtensions ce{env.app(), activeNoopJournal()};
            ce.cacheUNLReport(viewLedger);
            for (std::size_t i = 0; i < revealers; ++i)
            {
                auto const reveal =
                    sha512Half(vals[i].first, makeHash("tier2-reveal"));
                harvestCommitReveal(
                    ce,
                    calcNodeID(vals[i].first),
                    vals[i].first,
                    vals[i].second,
                    txSetHash,
                    seq,
                    closeTime,
                    prevLedger,
                    reveal);
            }
            ce.buildEntropySet(seq);
            CanonicalTXSet txs{makeHash("tier2-salt")};
            ce.onPreBuild(txs, seq, txSetHash);
            auto const tx = singleCanonicalTx(txs);
            std::pair<int, std::uint16_t> out{-1, 0};
            if (tx)
                out = {
                    tx->getFieldU8(sfEntropyTier),
                    tx->getFieldU16(sfEntropyCount)};
            return out;
        };

        // 5 of 6 aligned -> validator_quorum (count >= quorum 5).
        auto const q = runWith(5);
        BEAST_EXPECT(q.first == entropyTierValidatorQuorum);
        BEAST_EXPECT(q.second == 5);

        // 4 of 6 aligned -> participant_aligned (count >= tier2 4, < quorum 5).
        auto const p = runWith(4);
        BEAST_EXPECT(p.first == entropyTierParticipantAligned);
        BEAST_EXPECT(p.second == 4);

        // 3 of 6 aligned -> below the tier-2 floor -> consensus_fallback.
        auto const f = runWith(3);
        BEAST_EXPECT(f.first == entropyTierConsensusFallback);
        BEAST_EXPECT(f.second == 0);
    }

    void
    testTier2ThresholdAnchorsToOriginalView()
    {
        testcase(
            "Tier 2 threshold anchors to pre-nUNL original view under "
            "NegativeUNL");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy |
                featureNegativeUNL,
            nullptr};
        forceNonStandalone(env.app());

        // Ten active validators with two disabled via NegativeUNL: the
        // original-UNL denominator is 10 (Byzantine bound f = floor(10/5) = 2)
        // while the effective view is 8. Aligned cohorts form among the 8
        // effective validators, so two t-cohorts overlap by 2t - 8; safety
        // needs that overlap to exceed f so the two cohorts share an honest
        // validator. Tier 2 MUST key off the original view:
        //   tier2Threshold(original 10) == 7  -> overlap 2*7 - 8 = 6 > f: safe
        //   tier2Threshold(effective 8) == 5  -> overlap 2*5 - 8 = 2, NOT > f
        // A regression to size() would admit a 5-of-8 cohort as
        // participant_aligned, but its overlap (2) does not exceed the two
        // faulty validators the original UNL still tolerates, so an equivocator
        // could mint two distinct tier-2 digests: a fork. nUNL drops
        // honest-but-down nodes while leaving faulty ones, so the fault bound
        // stays anchored to the original UNL, not the shrunk effective view.
        // Under the correct anchor the band collapses (tier2 == quorum == 7)
        // and no tier-2 mint happens at all; this test pins the anchor so a
        // refactor to size() -- which re-opens the forkable [5, 7) band --
        // fails loudly.
        constexpr std::size_t kActive = 10;
        constexpr std::size_t kDisabled = 2;
        std::vector<PublicKey> activeKeys;
        activeKeys.reserve(kActive);
        for (std::size_t i = 0; i < kActive; ++i)
            activeKeys.push_back(randomKeyPair(KeyType::secp256k1).first);
        std::vector<PublicKey> const disabledKeys(
            activeKeys.begin(), activeKeys.begin() + kDisabled);

        auto const viewLedger =
            makeUNLReportLedger(env, activeKeys, disabledKeys);
        BEAST_EXPECT(viewLedger->rules().enabled(featureNegativeUNL));

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.cacheUNLReport(viewLedger);
        auto const view = ce.activeValidatorView();

        BEAST_EXPECT(view->fromUNLReport);
        BEAST_EXPECT(view->originalViewSize == kActive);    // 10, pre-nUNL
        BEAST_EXPECT(view->size() == kActive - kDisabled);  // 8, effective

        // The anchor. Tier 2 derives from originalViewSize (10 -> 7), NOT the
        // effective size (8 -> 5). The gate is min(quorum, tier2); quorum
        // tracks the effective view (8 -> 7), so under the correct anchor gate
        // == 7 and the band is closed, while a regression to size() would drop
        // the floor to 5 and the gate to min(7, 5) == 5.
        BEAST_EXPECT(calculateParticipantThreshold(kActive) == 7);
        BEAST_EXPECT(calculateParticipantThreshold(kActive - kDisabled) == 5);
        BEAST_EXPECT(ce.tier2Threshold() == 7);
        BEAST_EXPECT(ce.quorumThreshold() == 7);
        BEAST_EXPECT(ce.entropyGateThreshold() == 7);
    }

    void
    testOnPreBuildTier2WithNegativeUNL()
    {
        testcase("onPreBuild mints Tier 2 with active NegativeUNL");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy |
                featureNegativeUNL,
            nullptr};
        forceNonStandalone(env.app());

        // 20 original active validators, 1 disabled by nUNL:
        //   originalViewSize = 20 -> tier2Threshold = 13
        //   effective size   = 19 -> quorumThreshold = 16
        // so counts 13..15 are genuinely participant_aligned under active nUNL.
        constexpr std::size_t kOriginal = 20;
        constexpr std::size_t kDisabled = 1;
        std::vector<std::pair<PublicKey, SecretKey>> vals;
        vals.reserve(kOriginal);
        std::vector<PublicKey> activeKeys;
        activeKeys.reserve(kOriginal);
        for (std::size_t i = 0; i < kOriginal; ++i)
        {
            vals.push_back(randomKeyPair(KeyType::secp256k1));
            activeKeys.push_back(vals.back().first);
        }
        std::vector<PublicKey> const disabledKeys(
            activeKeys.begin(), activeKeys.begin() + kDisabled);
        auto const viewLedger =
            makeUNLReportLedger(env, activeKeys, disabledKeys);

        {
            ConsensusExtensions ce{env.app(), activeNoopJournal()};
            ce.cacheUNLReport(viewLedger);
            auto const view = ce.activeValidatorView();
            BEAST_EXPECT(view->fromUNLReport);
            BEAST_EXPECT(view->originalViewSize == kOriginal);
            BEAST_EXPECT(view->size() == kOriginal - kDisabled);
            BEAST_EXPECT(ce.tier2Threshold() == 13);
            BEAST_EXPECT(ce.quorumThreshold() == 16);
            BEAST_EXPECT(ce.entropyGateThreshold() == 13);
        }

        auto const anchor = env.app().getLedgerMaster().getClosedLedger();
        auto const prevLedger = anchor->info().hash;
        auto const seq = anchor->info().seq + 1;
        auto const closeTime = NetClock::time_point{NetClock::duration{778}};
        auto const txSetHash = makeHash("tier2-nunl-txset");

        auto runWith = [&](std::size_t revealers) {
            ConsensusExtensions ce{env.app(), activeNoopJournal()};
            ce.cacheUNLReport(viewLedger);
            for (std::size_t i = 0; i < revealers; ++i)
            {
                auto const idx = i + kDisabled;  // skip disabled validator
                auto const reveal =
                    sha512Half(vals[idx].first, makeHash("tier2-nunl-reveal"));
                harvestCommitReveal(
                    ce,
                    calcNodeID(vals[idx].first),
                    vals[idx].first,
                    vals[idx].second,
                    txSetHash,
                    seq,
                    closeTime,
                    prevLedger,
                    reveal);
            }
            ce.buildEntropySet(seq);
            CanonicalTXSet txs{makeHash("tier2-nunl-salt")};
            ce.onPreBuild(txs, seq, txSetHash);
            auto const tx = singleCanonicalTx(txs);
            std::pair<int, std::uint16_t> out{-1, 0};
            if (tx)
                out = {
                    tx->getFieldU8(sfEntropyTier),
                    tx->getFieldU16(sfEntropyCount)};
            return out;
        };

        auto const q = runWith(16);
        BEAST_EXPECT(q.first == entropyTierValidatorQuorum);
        BEAST_EXPECT(q.second == 16);

        auto const p = runWith(15);
        BEAST_EXPECT(p.first == entropyTierParticipantAligned);
        BEAST_EXPECT(p.second == 15);

        auto const f = runWith(12);
        BEAST_EXPECT(f.first == entropyTierConsensusFallback);
        BEAST_EXPECT(f.second == 0);
    }

    void
    testProposalProofRoundTrip()
    {
        testcase("proposal proof round-trip");

        auto const [publicKey, secretKey] = randomKeyPair(KeyType::secp256k1);
        auto const prevLedger = makeHash("proof-prev-ledger");
        auto const closeTime = NetClock::time_point{NetClock::duration{99}};
        ExtendedPosition position{makeHash("proof-tx-set")};
        position.myCommitment = makeHash("proof-commitment");

        auto const signature = signPosition(
            publicKey, secretKey, position, 0, closeTime, prevLedger);
        Serializer positionData;
        position.add(positionData);

        ConsensusExtensions::ProposalProof proof{
            0,
            static_cast<std::uint32_t>(closeTime.time_since_epoch().count()),
            prevLedger,
            std::move(positionData),
            signature};

        auto const blob = ConsensusExtensions::serializeProof(proof);
        auto parsed = ConsensusExtensions::deserializeProof(blob);
        BEAST_EXPECT(parsed);
        if (!parsed)
            return;
        BEAST_EXPECT(parsed->proposeSeq == proof.proposeSeq);
        BEAST_EXPECT(parsed->closeTime == proof.closeTime);
        BEAST_EXPECT(parsed->prevLedger == proof.prevLedger);
        BEAST_EXPECT(parsed->signature == proof.signature);
        BEAST_EXPECT(ConsensusExtensions::verifyProof(
            blob, publicKey, *position.myCommitment, true));
        BEAST_EXPECT(!ConsensusExtensions::verifyProof(
            blob, publicKey, makeHash("wrong-commitment"), true));

        Blob malformed{1, 2, 3};
        BEAST_EXPECT(!ConsensusExtensions::deserializeProof(malformed));
        BEAST_EXPECT(!ConsensusExtensions::verifyProof(
            malformed, publicKey, *position.myCommitment, true));
    }

    void
    testHarvestRngDataReplacementAndRejection()
    {
        testcase("harvestRngData replacement and rejection");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        auto const& publicKey = valKeys.keys->publicKey;
        auto const& secretKey = valKeys.keys->secretKey;
        auto const nodeId = valKeys.nodeID;
        auto const prevLedger = ledger->info().hash;
        auto const seq = ledger->seq() + 1;
        auto const closeTime = NetClock::time_point{NetClock::duration{654}};
        auto const txSetHash = makeHash("harvest-replace-txset");
        auto const reveal1 = makeHash("harvest-reveal-1");
        auto const reveal2 = makeHash("harvest-reveal-2");
        auto const commitment1 = sha512Half(reveal1, publicKey, seq);
        auto const commitment2 = sha512Half(reveal2, publicKey, seq);

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.cacheUNLReport(ledger);

        ExtendedPosition inactiveCommit{txSetHash};
        inactiveCommit.myCommitment = commitment1;
        ce.harvestRngData(
            makeNode(99),
            publicKey,
            inactiveCommit,
            0,
            closeTime,
            prevLedger,
            Slice{});
        BEAST_EXPECT(ce.pendingCommitCount() == 0);

        ExtendedPosition earlyReveal{txSetHash};
        earlyReveal.myReveal = reveal1;
        ce.harvestRngData(
            nodeId, publicKey, earlyReveal, 1, closeTime, prevLedger, Slice{});
        BEAST_EXPECT(ce.pendingRevealCount() == 0);

        ExtendedPosition commitPos{txSetHash};
        commitPos.myCommitment = commitment1;
        auto commitSig = signPosition(
            publicKey, secretKey, commitPos, 0, closeTime, prevLedger);
        ce.harvestRngData(
            nodeId,
            publicKey,
            commitPos,
            0,
            closeTime,
            prevLedger,
            Slice(commitSig.data(), commitSig.size()));
        BEAST_EXPECT(ce.pendingCommitCount() == 1);
        BEAST_EXPECT(ce.hasQuorumOfCommits());

        auto revealSig = signPosition(
            publicKey, secretKey, earlyReveal, 1, closeTime, prevLedger);
        ce.harvestRngData(
            nodeId,
            publicKey,
            earlyReveal,
            1,
            closeTime,
            prevLedger,
            Slice(revealSig.data(), revealSig.size()));
        BEAST_EXPECT(ce.pendingRevealCount() == 1);
        BEAST_EXPECT(ce.hasMinimumReveals());

        commitPos.myCommitment = commitment2;
        commitSig = signPosition(
            publicKey, secretKey, commitPos, 2, closeTime, prevLedger);
        ce.harvestRngData(
            nodeId,
            publicKey,
            commitPos,
            2,
            closeTime,
            prevLedger,
            Slice(commitSig.data(), commitSig.size()));
        BEAST_EXPECT(ce.pendingCommitCount() == 1);
        BEAST_EXPECT(ce.pendingRevealCount() == 0);
        BEAST_EXPECT(!ce.hasQuorumOfCommits());

        ce.harvestRngData(
            nodeId, publicKey, earlyReveal, 3, closeTime, prevLedger, Slice{});
        BEAST_EXPECT(ce.pendingRevealCount() == 0);

        ExtendedPosition reveal2Pos{txSetHash};
        reveal2Pos.myReveal = reveal2;
        revealSig = signPosition(
            publicKey, secretKey, reveal2Pos, 4, closeTime, prevLedger);
        ce.harvestRngData(
            nodeId,
            publicKey,
            reveal2Pos,
            4,
            closeTime,
            prevLedger,
            Slice(revealSig.data(), revealSig.size()));
        BEAST_EXPECT(ce.pendingRevealCount() == 1);
    }

    void
    testExportSidecarBuildFetchAndMerge()
    {
        testcase("Export sidecar build, fetch, and merge");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        auto const& valPK = valKeys.keys->publicKey;
        auto const& valSK = valKeys.keys->secretKey;
        auto const signerAccount = calcAccountID(valPK);
        auto const dst = calcAccountID(randomKeyPair(KeyType::secp256k1).first);
        auto const innerObj = makeExportedPayment(signerAccount, dst);
        auto const innerTx = makeSTTx(innerObj);
        auto const exportTx = makeExportTx(innerObj, signerAccount);
        auto const txHash = exportTx->getTransactionID();
        auto const txSet = makeRCLTxSet(env.app(), {exportTx});
        auto const seq = ledger->seq() + 1;

        ConsensusExtensions source{env.app(), activeNoopJournal()};
        source.setExportEnabledThisRound(true);
        source.cacheUNLReport(ledger);
        source.cacheConsensusTxSet(txSet);
        source.cacheConsensusTxSet(txSet);
        BEAST_EXPECT(source.hasConsensusExportTxns());
        BEAST_EXPECT(!source.hasPendingExportSigs());

        auto const sigData = buildMultiSigningData(innerTx, signerAccount);
        auto const sig = sign(valPK, valSK, sigData.slice());
        Buffer sigBuf(sig.data(), sig.size());
        source.exportSigCollector().addUnverifiedSignature(
            txHash, valPK, sigBuf, seq);
        BEAST_EXPECT(source.verifyPendingExportSigs(txSet, seq) == 1);
        BEAST_EXPECT(
            source.exportSigCollector().hasVerifiedSignature(txHash, valPK));
        BEAST_EXPECT(source.hasPendingExportSigs());

        auto const exportSigSetHash = source.buildExportSigSet(seq);
        BEAST_EXPECT(source.isSidecarSet(exportSigSetHash));

        ConsensusExtensions fetched{env.app(), activeNoopJournal()};
        fetched.setExportEnabledThisRound(true);
        fetched.cacheUNLReport(ledger);
        fetched.cacheConsensusTxSet(txSet);
        fetched.fetchRngSetIfNeeded(
            exportSigSetHash, ConsensusExtensions::SidecarKind::exportSig);

        BEAST_EXPECT(
            fetched.exportSigCollector().hasVerifiedSignature(txHash, valPK));
        BEAST_EXPECT(fetched.buildExportSigSet(seq) == exportSigSetHash);
    }

    void
    testExportAgreedSignaturesIgnoreLiveCollectorMutation()
    {
        testcase("Export apply uses agreed sidecar signatures");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        auto const& valPK = valKeys.keys->publicKey;
        auto const& valSK = valKeys.keys->secretKey;
        auto const signerAccount = calcAccountID(valPK);
        auto const dst = calcAccountID(randomKeyPair(KeyType::secp256k1).first);
        auto const innerObj = makeExportedPayment(signerAccount, dst);
        auto const innerTx = makeSTTx(innerObj);
        auto const exportTx = makeExportTx(innerObj, signerAccount);
        auto const txHash = exportTx->getTransactionID();
        auto const txSet = makeRCLTxSet(env.app(), {exportTx});
        auto const seq = ledger->seq() + 1;

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.setExportEnabledThisRound(true);
        ce.cacheUNLReport(ledger);
        ce.cacheConsensusTxSet(txSet);

        auto const sigData = buildMultiSigningData(innerTx, signerAccount);
        auto const sig = sign(valPK, valSK, sigData.slice());
        Buffer const originalSig(sig.data(), sig.size());
        ce.exportSigCollector().addVerifiedSignature(
            txHash, valPK, originalSig, seq);
        auto const exportSigSetHash = ce.buildExportSigSet(seq);
        BEAST_EXPECT(ce.isSidecarSet(exportSigSetHash));

        // Simulate a late local collector mutation after the sidecar hash has
        // converged. The live collector now differs from the agreed sidecar
        // map.
        std::uint8_t const lateBytes[] = {9, 8, 7};
        Buffer const lateSig{lateBytes, sizeof(lateBytes)};
        ce.exportSigCollector().addVerifiedSignature(
            txHash, valPK, lateSig, seq);

        auto const view = ce.activeValidatorView();
        auto const live = ce.exportSigCollector().checkQuorumAndSnapshot(
            txHash, 1, [&](PublicKey const& pk) {
                return ce.isActiveValidator(pk, *view);
            });
        BEAST_EXPECT(live);
        if (live)
            BEAST_EXPECT(live->at(valPK) == lateSig);

        auto const agreed =
            ce.agreedExportSignatures(*exportTx, txHash, *view, 1);
        BEAST_EXPECT(agreed);
        if (agreed)
        {
            BEAST_EXPECT(agreed->size() == 1);
            BEAST_EXPECT(agreed->at(valPK) == originalSig);
        }
    }

    void
    testOnPreBuildPreservesExportDecision()
    {
        testcase("onPreBuild preserves export state through buildLCL");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy | featureExport,
            nullptr};

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.setExportEnabledThisRound(true);
        ce.setRngEnabledThisRound(true);
        ce.setExportSigConvergenceFailed();
        auto const tx = makeHash("export-prebuild-preserve");
        auto const pk = makeValidatorKeys().front();
        std::uint8_t const sigBytes[] = {1, 2, 3};
        Buffer const sig{sigBytes, sizeof(sigBytes)};
        ce.exportSigCollector().addVerifiedSignature(tx, pk, sig, 10);

        CanonicalTXSet retriableTxs{makeHash("preserve-export-state")};
        ce.onPreBuild(retriableTxs, env.closed()->seq() + 1, makeHash("txset"));

        BEAST_EXPECT(ce.exportSigConvergenceFailed());
        BEAST_EXPECT(ce.exportSigCollector().signatureCount(tx) == 1);
    }

    void
    testRngSidecarBuildFetchAndMerge()
    {
        testcase("RNG sidecar build, fetch, and merge");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        auto const& publicKey = valKeys.keys->publicKey;
        auto const& secretKey = valKeys.keys->secretKey;
        auto const nodeId = valKeys.nodeID;
        auto const prevLedger = ledger->info().hash;
        auto const seq = ledger->seq() + 1;
        auto const closeTime = NetClock::time_point{NetClock::duration{777}};
        auto const txSetHash = makeHash("rng-sidecar-txset");
        auto const reveal = makeHash("rng-sidecar-reveal");
        auto const commitment = sha512Half(reveal, publicKey, seq);

        ConsensusExtensions source{env.app(), env.journal};
        source.cacheUNLReport(ledger);

        ExtendedPosition commitPos{txSetHash};
        commitPos.myCommitment = commitment;
        auto const commitSig = signPosition(
            publicKey, secretKey, commitPos, 0, closeTime, prevLedger);
        source.harvestRngData(
            nodeId,
            publicKey,
            commitPos,
            0,
            closeTime,
            prevLedger,
            Slice(commitSig.data(), commitSig.size()));
        BEAST_EXPECT(source.pendingCommitCount() == 1);

        ExtendedPosition revealPos{txSetHash};
        revealPos.myReveal = reveal;
        auto const revealSig = signPosition(
            publicKey, secretKey, revealPos, 1, closeTime, prevLedger);
        source.harvestRngData(
            nodeId,
            publicKey,
            revealPos,
            1,
            closeTime,
            prevLedger,
            Slice(revealSig.data(), revealSig.size()));
        BEAST_EXPECT(source.pendingRevealCount() == 1);

        auto const commitSetHash = source.buildCommitSet(seq);
        auto const revealSetHash = source.buildEntropySet(seq);
        BEAST_EXPECT(source.isSidecarSet(commitSetHash));
        BEAST_EXPECT(source.isSidecarSet(revealSetHash));

        ConsensusExtensions fetched{env.app(), env.journal};
        fetched.cacheUNLReport(ledger);
        fetched.fetchRngSetIfNeeded(
            commitSetHash, ConsensusExtensions::SidecarKind::commit);
        BEAST_EXPECT(fetched.pendingCommitCount() == 1);
        BEAST_EXPECT(fetched.buildCommitSet(seq) == commitSetHash);

        fetched.fetchRngSetIfNeeded(
            revealSetHash, ConsensusExtensions::SidecarKind::reveal);
        BEAST_EXPECT(fetched.pendingRevealCount() == 1);
        BEAST_EXPECT(fetched.buildEntropySet(seq) == revealSetHash);

        fetched.fetchRngSetIfNeeded(
            std::nullopt, ConsensusExtensions::SidecarKind::commit);
        fetched.fetchRngSetIfNeeded(
            uint256{}, ConsensusExtensions::SidecarKind::reveal);
        fetched.fetchRngSetIfNeeded(
            commitSetHash, ConsensusExtensions::SidecarKind::commit);
        fetched.fetchRngSetIfNeeded(
            revealSetHash, ConsensusExtensions::SidecarKind::reveal);

        auto const rawMap =
            makeRawSidecarSet(env.app(), std::string{"cached-sidecar"});
        auto const rawHash = rawMap->getHash().as_uint256();
        env.app().getInboundTransactions().giveSet(rawHash, rawMap, false);
        fetched.fetchRngSetIfNeeded(
            rawHash, ConsensusExtensions::SidecarKind::commit);
        BEAST_EXPECT(fetched.pendingCommitCount() == 1);

        fetched.setRngEnabledThisRound(true);
        fetched.recordParticipantDiagnostics(
            ConsensusMode::proposing, std::vector<NodeID>{nodeId});
        BEAST_EXPECT(fetched.observedParticipantCount() == 1);
        BEAST_EXPECT(fetched.observedParticipantsHash());
        BEAST_EXPECT(!fetched.observedParticipantsBitmapBin().empty());

        ExtendedPosition diagnosticPos{txSetHash};
        fetched.attachParticipantDiagnostics(diagnosticPos);
        BEAST_EXPECT(diagnosticPos.observedParticipantsHash);
    }

    void
    testRngSidecarRejectsInvalidFetchedEntries()
    {
        testcase("RNG sidecar rejects invalid fetched entries");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        auto const& publicKey = valKeys.keys->publicKey;
        auto const& secretKey = valKeys.keys->secretKey;
        auto const nodeId = valKeys.nodeID;
        auto const prevLedger = ledger->info().hash;
        auto const seq = ledger->seq() + 1;
        auto const closeTime = NetClock::time_point{NetClock::duration{777}};
        auto const txSetHash = makeHash("invalid-fetched-txset");
        auto const digest = makeHash("invalid-fetched-digest");

        auto makeRngSidecar = [&](std::uint8_t type,
                                  NodeID const& owner,
                                  PublicKey const& pk,
                                  uint256 const& value,
                                  LedgerIndex ledgerSeq) {
            STObject sidecar(sfGeneric);
            sidecar.setFieldU8(sfSidecarType, type);
            sidecar.setFieldU32(sfLedgerSequence, ledgerSeq);
            sidecar.setAccountID(sfAccount, accountFromNode(owner));
            sidecar.setFieldH256(sfDigest, value);
            sidecar.setFieldVL(sfSigningPubKey, pk.slice());
            return sidecar;
        };
        auto makeCommitProof = [&](uint256 const& value, std::uint32_t n = 0) {
            ExtendedPosition position{txSetHash};
            position.myCommitment = value;
            return makeProofBlob(
                publicKey, secretKey, position, n, closeTime, prevLedger);
        };
        auto makeRevealProof = [&](uint256 const& value, std::uint32_t n = 1) {
            ExtendedPosition position{txSetHash};
            position.myReveal = value;
            return makeProofBlob(
                publicKey, secretKey, position, n, closeTime, prevLedger);
        };

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.cacheUNLReport(ledger);

        // Commit sidecars need a verifiable proposal proof.
        publishAndFetchSidecarSet(
            env.app(),
            ce,
            makeSidecarSet(
                env.app(),
                {makeRngSidecar(
                    sidecarRngCommit, nodeId, publicKey, digest, seq)}),
            ConsensusExtensions::SidecarKind::commit);
        BEAST_EXPECT(ce.pendingCommitCount() == 0);

        // Entries from outside the active validator view are ignored.
        publishAndFetchSidecarSet(
            env.app(),
            ce,
            makeSidecarSet(
                env.app(),
                {makeRngSidecar(
                    sidecarRngReveal, makeNode(99), publicKey, digest, seq)}),
            ConsensusExtensions::SidecarKind::reveal);
        BEAST_EXPECT(ce.pendingRevealCount() == 0);

        // A valid active NodeID cannot be paired with an untrusted signing key.
        auto const [untrustedPk, _] = randomKeyPair(KeyType::secp256k1);
        publishAndFetchSidecarSet(
            env.app(),
            ce,
            makeSidecarSet(
                env.app(),
                {makeRngSidecar(
                    sidecarRngReveal, nodeId, untrustedPk, digest, seq)}),
            ConsensusExtensions::SidecarKind::reveal);
        BEAST_EXPECT(ce.pendingRevealCount() == 0);

        // Reveal sidecars are only valid after their matching commitment.
        publishAndFetchSidecarSet(
            env.app(),
            ce,
            makeSidecarSet(
                env.app(),
                {makeRngSidecar(
                    sidecarRngReveal, nodeId, publicKey, digest, seq)}),
            ConsensusExtensions::SidecarKind::reveal);
        BEAST_EXPECT(ce.pendingRevealCount() == 0);

        // Corrupt leaf bytes should not make the merge path throw outward.
        publishAndFetchSidecarSet(
            env.app(),
            ce,
            makeRawSidecarSet(env.app(), std::string{"not-an-stobject"}),
            ConsensusExtensions::SidecarKind::commit);
        BEAST_EXPECT(ce.pendingCommitCount() == 0);

        // A proof must verify the digest carried by the sidecar leaf.
        auto invalidProofSidecar =
            makeRngSidecar(sidecarRngCommit, nodeId, publicKey, digest, seq);
        invalidProofSidecar.setFieldVL(
            sfBlob, makeCommitProof(makeHash("other-commitment")));
        publishAndFetchSidecarSet(
            env.app(),
            ce,
            makeSidecarSet(env.app(), {invalidProofSidecar}),
            ConsensusExtensions::SidecarKind::commit);
        BEAST_EXPECT(ce.pendingCommitCount() == 0);

        // verifyProof ignores trailing bytes, but deserializeProof rejects them
        // before caching the proof for deterministic sidecar rebuilds.
        auto malformedProof = makeCommitProof(digest);
        malformedProof.push_back(0);
        auto malformedProofSidecar =
            makeRngSidecar(sidecarRngCommit, nodeId, publicKey, digest, seq);
        malformedProofSidecar.setFieldVL(sfBlob, malformedProof);
        publishAndFetchSidecarSet(
            env.app(),
            ce,
            makeSidecarSet(env.app(), {malformedProofSidecar}),
            ConsensusExtensions::SidecarKind::commit);
        BEAST_EXPECT(ce.pendingCommitCount() == 0);

        auto outOfRoundSidecar = makeRngSidecar(
            sidecarRngCommit, nodeId, publicKey, digest, seq + 1);
        outOfRoundSidecar.setFieldVL(sfBlob, makeCommitProof(digest));
        publishAndFetchSidecarSet(
            env.app(),
            ce,
            makeSidecarSet(env.app(), {outOfRoundSidecar}),
            ConsensusExtensions::SidecarKind::commit);
        BEAST_EXPECT(ce.pendingCommitCount() == 0);

        auto nonZeroProofSidecar =
            makeRngSidecar(sidecarRngCommit, nodeId, publicKey, digest, seq);
        nonZeroProofSidecar.setFieldVL(sfBlob, makeCommitProof(digest, 2));
        publishAndFetchSidecarSet(
            env.app(),
            ce,
            makeSidecarSet(env.app(), {nonZeroProofSidecar}),
            ConsensusExtensions::SidecarKind::commit);
        BEAST_EXPECT(ce.pendingCommitCount() == 1);
        BEAST_EXPECT(
            ce.buildCommitSet(seq) !=
            makeSidecarSet(env.app(), {nonZeroProofSidecar})
                ->getHash()
                .as_uint256());

        ConsensusExtensions replacement{env.app(), activeNoopJournal()};
        replacement.cacheUNLReport(ledger);
        auto const reveal1 = makeHash("fetched-reveal-1");
        auto const reveal2 = makeHash("fetched-reveal-2");
        auto const commit1 = sha512Half(reveal1, publicKey, seq);
        auto const commit2 = sha512Half(reveal2, publicKey, seq);

        auto commit1Sidecar =
            makeRngSidecar(sidecarRngCommit, nodeId, publicKey, commit1, seq);
        commit1Sidecar.setFieldVL(sfBlob, makeCommitProof(commit1));
        publishAndFetchSidecarSet(
            env.app(),
            replacement,
            makeSidecarSet(env.app(), {commit1Sidecar}),
            ConsensusExtensions::SidecarKind::commit);
        BEAST_EXPECT(replacement.pendingCommitCount() == 1);

        auto reveal1Sidecar =
            makeRngSidecar(sidecarRngReveal, nodeId, publicKey, reveal1, seq);
        reveal1Sidecar.setFieldVL(sfBlob, makeRevealProof(reveal1));
        publishAndFetchSidecarSet(
            env.app(),
            replacement,
            makeSidecarSet(env.app(), {reveal1Sidecar}),
            ConsensusExtensions::SidecarKind::reveal);
        BEAST_EXPECT(replacement.pendingRevealCount() == 1);

        auto commit2Sidecar =
            makeRngSidecar(sidecarRngCommit, nodeId, publicKey, commit2, seq);
        commit2Sidecar.setFieldVL(sfBlob, makeCommitProof(commit2));
        publishAndFetchSidecarSet(
            env.app(),
            replacement,
            makeSidecarSet(env.app(), {commit2Sidecar}),
            ConsensusExtensions::SidecarKind::commit);
        BEAST_EXPECT(replacement.pendingCommitCount() == 1);
        BEAST_EXPECT(replacement.pendingRevealCount() == 0);

        // The old reveal is no longer valid after the commitment changes.
        publishAndFetchSidecarSet(
            env.app(),
            replacement,
            makeSidecarSet(env.app(), {reveal1Sidecar}),
            ConsensusExtensions::SidecarKind::reveal);
        BEAST_EXPECT(replacement.pendingRevealCount() == 0);

        // With a local map already built, fetched deltas merge only missing
        // leaves; corrupt remote additions are ignored without disturbing local
        // state.
        replacement.buildCommitSet(seq);
        publishAndFetchSidecarSet(
            env.app(),
            replacement,
            makeRawSidecarSet(env.app(), std::string{"bad-diff-entry"}),
            ConsensusExtensions::SidecarKind::commit);
        BEAST_EXPECT(replacement.pendingCommitCount() == 1);
    }

    void
    testOnPreBuildInjectsStandaloneEntropy()
    {
        testcase("onPreBuild injects standalone entropy pseudo-tx");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        CanonicalTXSet retriableTxs{makeHash("rng-on-pre-build-salt")};
        auto const seq = env.closed()->seq() + 1;

        ce.onPreBuild(retriableTxs, seq, makeHash("standalone-txset"));
        BEAST_EXPECT(
            std::distance(retriableTxs.begin(), retriableTxs.end()) == 1);

        auto const tx = retriableTxs.begin()->second;
        BEAST_EXPECT(tx);
        BEAST_EXPECT(tx->getTxnType() == ttCONSENSUS_ENTROPY);
        BEAST_EXPECT(tx->getFieldU32(sfLedgerSequence) == seq);
        BEAST_EXPECT(tx->getAccountID(sfAccount) == AccountID{});
        BEAST_EXPECT(
            tx->getFieldH256(sfDigest) ==
            sha512Half(std::string("standalone-entropy"), seq));
        BEAST_EXPECT(tx->getFieldU16(sfEntropyCount) == 20);
        BEAST_EXPECT(
            tx->getFieldU8(sfEntropyTier) == entropyTierValidatorQuorum);

        // Value-based dedup: re-injecting with identical inputs yields the
        // identical pseudo-tx (same txID) -> verified skip, still one entry.
        ce.onPreBuild(retriableTxs, seq, makeHash("standalone-txset"));
        BEAST_EXPECT(
            std::distance(retriableTxs.begin(), retriableTxs.end()) == 1);
    }

    void
    testOnPreBuildEntropyMismatchKeepsAgreed()
    {
        testcase("onPreBuild keeps a present-but-different entropy pseudo-tx");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        auto const seq = env.closed()->seq() + 1;

        // Build a ttCONSENSUS_ENTROPY, optionally without sfEntropyTier
        // (mimics a pre-tier-3 / malformed entry).
        auto makeEntropyTx = [&](uint256 const& digest, std::uint16_t count) {
            STObject obj(sfGeneric);
            obj.setFieldU16(sfTransactionType, ttCONSENSUS_ENTROPY);
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setAccountID(sfAccount, AccountID{});
            obj.setFieldU32(sfSequence, 0);
            obj.setFieldAmount(sfFee, STAmount{});
            obj.setFieldVL(sfSigningPubKey, Slice{});  // pseudo-tx convention
            obj.setFieldH256(sfDigest, digest);
            obj.setFieldU16(sfEntropyCount, count);
            obj.setFieldU8(sfEntropyTier, entropyTierConsensusFallback);
            return std::make_shared<STTx const>(makeSTTx(obj));
        };

        // (1) Well-formed but DIFFERENT digest already present: standalone
        // injection would produce its own digest, so txIDs differ -> mismatch
        // branch keeps the agreed one, does not insert ours, stays at one.
        {
            ConsensusExtensions ce{env.app(), activeNoopJournal()};
            CanonicalTXSet txs{makeHash("mismatch-wellformed-salt")};
            auto const present =
                makeEntropyTx(makeHash("a-different-digest"), 7);
            auto const presentID = present->getTransactionID();
            txs.insert(present);

            ce.onPreBuild(txs, seq, makeHash("mismatch-txset"));

            BEAST_EXPECT(std::distance(txs.begin(), txs.end()) == 1);
            auto const kept = singleCanonicalTx(txs);
            BEAST_EXPECT(kept);
            if (kept)
            {
                BEAST_EXPECT(kept->getTransactionID() == presentID);
                BEAST_EXPECT(
                    kept->getFieldH256(sfDigest) ==
                    makeHash("a-different-digest"));
            }
        }

        // Note: a ttCONSENSUS_ENTROPY missing sfEntropyTier cannot be
        // constructed (STTx deserialization enforces all soeREQUIRED fields),
        // so the mismatch-branch's defensive isFieldPresent reads guard an
        // unreachable-via-tx-path case — belt-and-suspenders, kept cheap.
    }

    void
    testDiagnosticsJsonAndPositionLogging()
    {
        testcase("diagnostics JSON and position logging");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        ce.setRngEnabledThisRound(true);
        ce.recordParticipantDiagnostics(
            ConsensusMode::proposing, std::vector<NodeID>{valKeys.nodeID});

        Json::Value json;
        ce.appendJson(json);
        BEAST_EXPECT(json.isMember("rng"));
        BEAST_EXPECT(json["rng"]["enabled"].asBool());
        BEAST_EXPECT(json["rng"]["est_state"].asString() == "ConvergingTx");
        BEAST_EXPECT(
            json["rng"]["observed_active_participants"].asInt() ==
            static_cast<int>(ce.observedParticipantCount()));
        BEAST_EXPECT(json["rng"].isMember("observed_participants"));
        BEAST_EXPECT(json["rng"].isMember("observed_participants_bitmap"));

        ExtendedPosition pos{makeHash("diagnostic-tx-set")};
        pos.commitSetHash = makeHash("diagnostic-commit-set");
        pos.entropySetHash = makeHash("diagnostic-entropy-set");
        pos.exportSigSetHash = makeHash("diagnostic-export-set");
        pos.exportSignaturesHash = makeHash("diagnostic-export-signatures");
        pos.observedParticipantsHash = ce.observedParticipantsHash();
        pos.myCommitment = makeHash("diagnostic-commitment");
        pos.myReveal = makeHash("diagnostic-reveal");
        ce.logPosition(pos, activeNoopJournal());
    }

    void
    testDecoratePositionSkipsWhenDisabled()
    {
        testcase("decoratePosition skips without amendment");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ExtendedPosition pos{makeHash("decorate-position-tx-set")};

        ce.decoratePosition(pos, ledger, true);
        BEAST_EXPECT(!pos.myCommitment);
        BEAST_EXPECT(ce.pendingCommitCount() == 0);

        ExtendedPosition skipped{makeHash("decorate-position-skipped")};
        ce.decoratePosition(skipped, ledger, false);
        BEAST_EXPECT(!skipped.myCommitment);
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
        // can split validator entropy from deterministic consensus_fallback.
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
    testRngBootstrapSkipWhenPreviousParticipantsBelowGate()
    {
        testcase("RNG bootstrap skip below previous participant entropy gate");

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
    testRngCommitTimeoutWithEntropyGatePublishesCommitSet()
    {
        testcase("RNG commit timeout with entropy gate publishes commit set");

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
    testExportSigGateAllowsQuorumDespiteMissingObservation()
    {
        testcase(
            "Export sig gate allows quorum despite missing sidecar "
            "observation");

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

        // A quorum-aligned signed exportSigSetHash is enough even if a
        // tx-converged minority peer has not advertised any exportSigSetHash.
        result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(!ext.exportSigConvergenceFailed_);
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

    void
    testWireProposalHarvestsExportSigs()
    {
        testcase("wire proposal harvests export signatures");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.setExportEnabledThisRound(true);
        ce.cacheUNLReport();

        protocol::TMProposeSet wire;
        auto const& senderPK = valKeys.keys->publicKey;
        wire.set_nodepubkey(senderPK.data(), senderPK.size());
        auto const prevLedger = *ce.activeValidatorView()->sourceLedgerHash;
        wire.set_previousledger(prevLedger.data(), prevLedger.size());
        auto const tx = makeHash("wire-export-sig-tx");
        auto const blob = makeExportSigBlob(tx, senderPK);
        wire.add_exportsignatures(blob);

        ce.onTrustedPeerMessage(wire);
        BEAST_EXPECT(ce.exportSigCollector().hasUnverifiedSignatures());
        auto const beforeMalformed =
            ce.exportSigCollector().unverifiedSignatures(tx);
        BEAST_EXPECT(beforeMalformed.size() == 1);

        protocol::TMProposeSet malformed;
        malformed.add_exportsignatures(blob);
        malformed.set_nodepubkey("bad", 3);
        ce.onTrustedPeerMessage(malformed);
        auto const afterMalformed =
            ce.exportSigCollector().unverifiedSignatures(tx);
        BEAST_EXPECT(afterMalformed == beforeMalformed);
    }

    void
    testPublicHookNoopAndFailureBranches()
    {
        testcase("public hook no-op and failure branches");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        ConsensusExtensions ce{env.app(), activeNoopJournal()};

        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        ce.cacheUNLReport(ledger);
        BEAST_EXPECT(ce.exportSigQuorumThreshold() == 1);

        ce.setExportSigConvergenceFailed();
        BEAST_EXPECT(ce.exportSigConvergenceFailed());

        ce.setEntropyFailed();

        ce.generateEntropySecret();
        BEAST_EXPECT(!ce.hasAnyReveals());
        ce.selfSeedReveal();
        BEAST_EXPECT(ce.pendingRevealCount() == 1);

        auto const [pk, _] = randomKeyPair(KeyType::secp256k1);
        std::vector<std::string> const noSigs;
        BEAST_EXPECT(
            ce.harvestExportSignatures(
                pk, ledger->info().hash, noSigs, "disabled") == 0);

        ce.setExportEnabledThisRound(true);
        BEAST_EXPECT(
            ce.harvestExportSignatures(
                pk, ledger->info().hash, noSigs, "empty") == 0);

        protocol::TMProposeSet wire;
        ce.onTrustedPeerMessage(wire);

        wire.add_exportsignatures(
            makeExportSigBlob(makeHash("wire-no-prev"), pk));
        wire.set_nodepubkey(pk.data(), pk.size());
        ce.onTrustedPeerMessage(wire);

        Json::Value json;
        ce.estState_ = EstablishState::ConvergingCommit;
        ce.appendJson(json);
        BEAST_EXPECT(json["rng"]["est_state"].asString() == "ConvergingCommit");
        ce.estState_ = EstablishState::ConvergingReveal;
        ce.appendJson(json);
        BEAST_EXPECT(json["rng"]["est_state"].asString() == "ConvergingReveal");

        ExtendedPosition logPos{makeHash("log-inactive-position")};
        ce.logPosition(
            logPos,
            beast::Journal{beast::Journal::getNullSink()},
            beast::severities::kTrace);

        protocol::TMProposeSet prop;
        RCLCxPeerPos::Proposal proposal{
            ledger->info().hash,
            0,
            ExtendedPosition{makeHash("attach-export-disabled")},
            NetClock::time_point{},
            NetClock::time_point{},
            env.app().getValidatorKeys().nodeID};
        ConsensusExtensions disabled{env.app(), activeNoopJournal()};
        disabled.attachExportSignatures(prop, proposal);
        BEAST_EXPECT(prop.exportsignatures_size() == 0);

        ConsensusTestConfig cfg;
        cfg.noExportSig = true;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);
        ce.attachExportSignatures(prop, proposal);
        BEAST_EXPECT(prop.exportsignatures_size() == 0);
    }

    void
    testDecorateMessageStoresSelfProofs()
    {
        testcase("decorateMessage stores self proofs");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy,
            nullptr};
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ExtendedPosition commitPos{makeHash("decorate-message-txset")};
        ce.decoratePosition(commitPos, ledger, true);
        BEAST_EXPECT(commitPos.myCommitment);
        if (!commitPos.myCommitment)
            return;

        auto const closeTime = NetClock::time_point{NetClock::duration{12345}};
        RCLCxPeerPos::Proposal commitProposal{
            ledger->info().hash,
            0,
            commitPos,
            closeTime,
            NetClock::time_point{},
            valKeys.nodeID};
        auto const commitSig = signDigest(
            valKeys.keys->publicKey,
            valKeys.keys->secretKey,
            commitProposal.signingHash());
        protocol::TMProposeSet prop;
        ce.decorateMessage(
            prop,
            commitProposal,
            commitPos,
            Buffer(commitSig.data(), commitSig.size()));

        auto const seq = ledger->info().seq + 1;
        auto const commitHash = ce.buildCommitSet(seq);
        BEAST_EXPECT(ce.isSidecarSet(commitHash));

        ExtendedPosition revealPos{commitPos.txSetHash};
        revealPos.myReveal = ce.getEntropySecret();
        RCLCxPeerPos::Proposal revealProposal{
            ledger->info().hash,
            1,
            revealPos,
            closeTime,
            NetClock::time_point{},
            valKeys.nodeID};
        auto const revealSig = signDigest(
            valKeys.keys->publicKey,
            valKeys.keys->secretKey,
            revealProposal.signingHash());
        ce.decorateMessage(
            prop,
            revealProposal,
            revealPos,
            Buffer(revealSig.data(), revealSig.size()));

        BEAST_EXPECT(ce.pendingRevealCount() == 1);
        BEAST_EXPECT(ce.hasMinimumReveals());
        auto const entropyHash = ce.buildEntropySet(seq);
        BEAST_EXPECT(ce.isSidecarSet(entropyHash));
    }

public:
    void
    run() override
    {
        testSidecarPeerAlignmentHelper();
        testActiveValidatorViewBuilderPrefersUNLReport();
        testActiveValidatorViewBuilderFallback();
        testActiveValidatorViewAppliesNegativeUNL();
        testActiveValidatorViewNullSourceAndExpectedProposers();
        testParticipantThreshold();
        testRuntimeConfigPolicyAccessors();
        testDecoratePositionGeneratesCommitment();
        testOnPreBuildInjectsZeroEntropyFallback();
        testOnPreBuildInjectsEntropySetEntropy();
        testOnPreBuildTier2ParticipantAligned();
        testTier2ThresholdAnchorsToOriginalView();
        testOnPreBuildTier2WithNegativeUNL();
        testProposalProofRoundTrip();
        testHarvestRngDataReplacementAndRejection();
        testExportSidecarBuildFetchAndMerge();
        testExportAgreedSignaturesIgnoreLiveCollectorMutation();
        testOnPreBuildPreservesExportDecision();
        testRngSidecarBuildFetchAndMerge();
        testRngSidecarRejectsInvalidFetchedEntries();
        testOnPreBuildInjectsStandaloneEntropy();
        testOnPreBuildEntropyMismatchKeepsAgreed();
        testDiagnosticsJsonAndPositionLogging();
        testDecoratePositionSkipsWhenDisabled();
        testExportSigGateRequiresQuorumAlignment();
        testRngEntropyGateRequiresFullObservation();
        testRngFastPathWaitsAfterEntropyPublish();
        testRngBootstrapSkipWhenPreviousParticipantsBelowGate();
        testRngCommitWaitsWhenQuorumPossible();
        testRngCommitTimeoutWithEntropyGatePublishesCommitSet();
        testRngCommitQuorumInObservingModeDoesNotPropose();
        testRngCommitConflictRefreshesHashBeforeWaiting();
        testRngCommitHashConflictTimeoutFallsBack();
        testRngRevealTransitionWaitsWhenRevealsIncomplete();
        testRngRevealTimeoutWithoutRevealsFallsBack();
        testRngEntropyPublishIsIdempotent();
        testRngEntropyConflictTimeoutClearsHash();
        testRngEntropyConflictRefreshesHashBeforeWaiting();
        testRngEntropyConflictIgnoredWithQuorumAlignment();
        testExportSigGateAllowsAlignedQuorumDespiteMinorityConflict();
        testExportSigGateAllowsQuorumDespiteMissingObservation();
        testExportSigGateFetchesAdvertisedPeerSets();
        testExportSigGateObservingModeDoesNotPropose();
        testExportSigGateRefreshesHashBeforeWaiting();
        testExportSigGateBoundsCandidateObservationWindow();
        testExportSigGateSkipsWhenExportDisabled();
        testParticipantDiagnosticsOnlyWhenExtensionEnabled();
        testExportDisabledRoundClearsCollector();
        testReplayedProposalHarvestsExportSigs();
        testWireProposalHarvestsExportSigs();
        testPublicHookNoopAndFailureBranches();
        testDecorateMessageStoresSelfProofs();
    }
};

BEAST_DEFINE_TESTSUITE(ConsensusExtensions, consensus, ripple);

}  // namespace test
}  // namespace ripple
