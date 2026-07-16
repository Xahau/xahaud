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
#include <test/jtx/WSClient.h>
#include <xrpld/app/consensus/ActiveValidatorView.h>
#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/consensus/ProposalPrecheck.h>
#include <xrpld/app/ledger/BuildLedger.h>
#include <xrpld/app/ledger/InboundTransactions.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/detail/TransactionAcquire.h>
#include <xrpld/app/main/CollectorManager.h>
#include <xrpld/app/misc/CanonicalTXSet.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/misc/NegativeUNLVote.h>
#include <xrpld/app/misc/RuntimeConfig.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpld/consensus/ConsensusExtensionsTick.h>
#include <xrpld/consensus/ConsensusProposal.h>
#include <xrpld/ledger/Sandbox.h>
#include <xrpld/overlay/PeerSet.h>
#include <xrpld/shamap/SHAMapSidecarLeafNode.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/EntropyTier.h>
#include <xrpl/protocol/ExportCommittee.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/ExportOriginMemo.h>
#include <xrpl/protocol/ExportShare.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SidecarType.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <tuple>

namespace ripple {
namespace test {

namespace {

Manifest
makeValidatorManifest(
    SecretKey const& masterSecret,
    SecretKey const& signingSecret,
    std::uint32_t sequence = 0)
{
    auto const master = derivePublicKey(KeyType::secp256k1, masterSecret);
    auto const signing = derivePublicKey(KeyType::secp256k1, signingSecret);
    STObject object{sfGeneric};
    object.setFieldU32(sfSequence, sequence);
    object.setFieldVL(sfPublicKey, master.slice());
    object.setFieldVL(sfSigningPubKey, signing.slice());
    sign(object, HashPrefix::manifest, KeyType::secp256k1, signingSecret);
    sign(
        object,
        HashPrefix::manifest,
        KeyType::secp256k1,
        masterSecret,
        sfMasterSignature);

    Serializer serialized;
    object.add(serialized);
    auto manifest = deserializeManifest(std::string{
        static_cast<char const*>(serialized.data()), serialized.size()});
    if (!manifest)
        Throw<std::runtime_error>("failed to build validator manifest");
    return std::move(*manifest);
}

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

AccountID
accountFromNode(NodeID const& nodeId)
{
    AccountID account;
    std::memcpy(account.data(), nodeId.data(), account.size());
    return account;
}

Blob
expectedContributorMask(
    std::vector<PublicKey> activeMasterKeys,
    std::vector<NodeID> const& contributorNodes)
{
    std::sort(activeMasterKeys.begin(), activeMasterKeys.end());
    Blob mask((activeMasterKeys.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < activeMasterKeys.size(); ++i)
    {
        auto const nodeId = calcNodeID(activeMasterKeys[i]);
        if (std::find(
                contributorNodes.begin(), contributorNodes.end(), nodeId) ==
            contributorNodes.end())
            continue;
        mask[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
    }
    return mask;
}

Blob
standaloneContributorMask(std::uint16_t denominator, std::uint16_t count)
{
    Blob mask((denominator + 7) / 8, 0);
    for (std::uint16_t i = 0; i < std::min(denominator, count); ++i)
        mask[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
    return mask;
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
makeExportedPayment(
    AccountID const& src,
    AccountID const& dst,
    std::uint32_t ticketSequence = 1)
{
    STObject obj(sfExportedTxn);
    obj.setFieldU16(sfTransactionType, ttPAYMENT);
    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);
    obj.setFieldU32(sfSequence, 0);
    obj.setFieldU32(sfTicketSequence, ticketSequence);
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
makeConsensusEntropyTx(
    std::uint32_t ledgerSeq,
    uint256 const& digest,
    std::uint16_t count)
{
    STObject obj(sfGeneric);
    obj.setFieldU16(sfTransactionType, ttCONSENSUS_ENTROPY);
    obj.setFieldU32(sfLedgerSequence, ledgerSeq);
    obj.setAccountID(sfAccount, AccountID{});
    obj.setFieldU32(sfSequence, 0);
    obj.setFieldAmount(sfFee, STAmount{});
    obj.setFieldVL(sfSigningPubKey, Slice{});
    obj.setFieldH256(sfDigest, digest);
    obj.setFieldU16(sfEntropyCount, count);
    obj.setFieldU16(sfEntropyDenominator, count);
    obj.setFieldVL(
        sfEntropyContributors, standaloneContributorMask(count, count));
    obj.setFieldU8(sfEntropyTier, entropyTierValidatorFull);
    return std::make_shared<STTx const>(makeSTTx(obj));
}

std::shared_ptr<STTx const>
makeExportSignaturesTx(std::uint32_t ledgerSeq, uint256 const& exportTxHash)
{
    auto const signer = randomKeyPair(KeyType::secp256k1);
    auto const dst = calcAccountID(randomKeyPair(KeyType::secp256k1).first);
    auto const innerObj = makeExportedPayment(calcAccountID(signer.first), dst);
    auto const innerTx = makeSTTx(innerObj);

    ExportResultBuilder::PositionedSignatureSnapshot signatures;
    auto sig = ExportResultBuilder::signExportedTxn(
        innerTx, signer.first, signer.second);
    signatures.emplace(
        0,
        ExportResultBuilder::PositionedSignature{
            signer.first, Buffer(sig.data(), sig.size())});

    return std::make_shared<STTx const>(
        ExportResultBuilder::buildSignatureWitness(
            exportTxHash, innerTx, signatures, 1, ledgerSeq));
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
expectedEntropy(
    std::vector<std::pair<PublicKey, uint256>> const& orderedContributions)
{
    Serializer s;
    for (auto const& [key, reveal] : orderedContributions)
    {
        s.addVL(key.slice());
        s.addBitString(reveal);
    }
    return sha512Half(s.slice());
}

uint256
expectedEntropy(PublicKey const& key, uint256 const& reveal)
{
    return expectedEntropy({{key, reveal}});
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
    bool livePendingExportLatches{false};
    bool exportOn{true};
    bool entropyFailed{false};
    bool commitFrozen{false};
    std::size_t sidecarQuorum{4};
    std::size_t commits{4};
    std::size_t proofedCommits{4};
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

    bool
    testSuppressExportSigSetHash() const
    {
        return false;
    }

    std::size_t
    quorumThreshold() const
    {
        return sidecarQuorum;
    }

    std::size_t
    entropyGateThreshold() const
    {
        // Stub default mirrors quorumThreshold (tier-2 band collapsed); the
        // tier-2 step-down is exercised end-to-end in the CSF sims.
        return sidecarQuorum;
    }

    std::size_t
    exportRootAlignmentThreshold() const
    {
        return sidecarQuorum;
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
    proofedCommitCount() const
    {
        return rngOn ? proofedCommits : 0;
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

    void
    acceptEntropySet(uint256 const&)
    {
    }

    void
    clearAcceptedEntropySet()
    {
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
    freezeRngCommitSet()
    {
        commitFrozen = true;
    }

    bool
    hasPendingExportSigs() const
    {
        return localExportSigs;
    }

    bool
    hasEligiblePendingExports() const
    {
        return livePendingExportLatches;
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

    void
    acceptExportSigSet(uint256 const&)
    {
    }

    void
    clearAcceptedExportSigSet()
    {
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

struct ExtensionTickHarness
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

        BEAST_EXPECT(detail::sidecarLocalContribution(true) == 1);
        BEAST_EXPECT(detail::sidecarLocalContribution(false) == 0);
        BEAST_EXPECT(detail::sidecarLocalContribution(true, true) == 1);
        BEAST_EXPECT(detail::sidecarLocalContribution(true, false) == 0);
        BEAST_EXPECT(detail::sidecarLocalContribution(false, true) == 0);
        BEAST_EXPECT(detail::sidecarAlignedParticipants(2, true) == 3);
        BEAST_EXPECT(detail::sidecarAlignedParticipants(2, false) == 2);
        BEAST_EXPECT(detail::sidecarQuorumAligned(2, true, 3));
        BEAST_EXPECT(!detail::sidecarQuorumAligned(2, true, 4));
        BEAST_EXPECT(detail::sidecarFullObservation(2, 2));
        BEAST_EXPECT(!detail::sidecarFullObservation(2, 3));

        ExtensionTickHarness harness;
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

        std::vector<uint256> mismatches;
        auto const state = detail::inspectTxConvergedSidecarPeers(
            harness.peers,
            harness.position,
            true,
            true,
            exportHashOf,
            allMembers,
            [&](auto const& hash) {
                if (hash)
                    mismatches.push_back(*hash);
            });

        BEAST_EXPECT(state.localCounts);
        BEAST_EXPECT(state.conflict);
        BEAST_EXPECT(state.aligned == 1);
        BEAST_EXPECT(state.alignedParticipants() == 2);
        BEAST_EXPECT(state.peersSeen == 2);
        BEAST_EXPECT(state.txConverged == 3);
        BEAST_EXPECT(state.quorumAligned(2));
        BEAST_EXPECT(!state.quorumAligned(3));
        BEAST_EXPECT(!state.fullObservation());
        BEAST_EXPECT(mismatches.size() == 1);
        if (!mismatches.empty())
            BEAST_EXPECT(mismatches.front() == conflictHash);

        harness.position.exportSigSetHash.reset();
        auto const unpublishedState = detail::inspectTxConvergedSidecarPeers(
            harness.peers,
            harness.position,
            true,
            false,
            exportHashOf,
            allMembers,
            [](auto const&) {});
        BEAST_EXPECT(!unpublishedState.localCounts);
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
            true,
            exportHashOf,
            allMembers,
            [](auto const&) {});
        BEAST_EXPECT(padded.aligned == 2);  // node 1 + node 5, unfiltered

        auto const filtered = detail::inspectTxConvergedSidecarPeers(
            harness.peers,
            harness.position,
            true,
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
            true,
            exportHashOf,
            activeOnly,
            [](auto const&) {});
        BEAST_EXPECT(!nonActiveLocal.localCounts);
        BEAST_EXPECT(nonActiveLocal.alignedParticipants() == 1);  // node 1 only
        //@@end test-sidecar-active-view-filter
    }

    void
    testSidecarSplitBrainEquivocationThreshold()
    {
        testcase("Sidecar split-brain equivocation threshold");

        auto const hashA = makeHash("split-brain-sidecar-a");
        auto const hashB = makeHash("split-brain-sidecar-b");
        auto const exportHashOf = [](auto const& position) {
            return position.exportSigSetHash;
        };
        auto const allMembers = [](auto const&) { return true; };

        auto inspect = [&](std::uint8_t localId,
                           uint256 const& localHash,
                           std::vector<std::uint8_t> const& hashANodes,
                           std::vector<std::uint8_t> const& hashBNodes) {
            ExtensionTickHarness harness;
            harness.position.exportSigSetHash = localHash;
            auto addPeer = [&](std::uint8_t id, uint256 const& hash) {
                if (id != localId)
                    harness.addPeer(id, hash);
            };
            for (auto id : hashANodes)
                addPeer(id, hashA);
            for (auto id : hashBNodes)
                addPeer(id, hashB);

            return detail::inspectTxConvergedSidecarPeers(
                harness.peers,
                harness.position,
                true,
                true,
                exportHashOf,
                allMembers,
                [](auto const&) {});
        };

        // n=9, f=floor(n/5)=1. One equivocator (node 8) can show hashA to
        // nodes 0..3 and hashB to nodes 4..7. A plain strict-majority gate
        // (5/9) would let both local views proceed on different hashes. The
        // participant threshold is 6, so neither split cohort can pass.
        {
            auto const viewA = inspect(0, hashA, {0, 1, 2, 3, 8}, {4, 5, 6, 7});
            auto const viewB = inspect(4, hashB, {0, 1, 2, 3}, {4, 5, 6, 7, 8});
            auto const strictMajority = std::size_t{5};
            auto const participant = calculateParticipantThreshold(9);

            BEAST_EXPECT(viewA.alignedParticipants() == strictMajority);
            BEAST_EXPECT(viewB.alignedParticipants() == strictMajority);
            BEAST_EXPECT(viewA.quorumAligned(strictMajority));
            BEAST_EXPECT(viewB.quorumAligned(strictMajority));
            BEAST_EXPECT(participant == 6);
            BEAST_EXPECT(!viewA.quorumAligned(participant));
            BEAST_EXPECT(!viewB.quorumAligned(participant));
        }

        // n=10, f=2. Two equivocators (8,9) make two disjoint honest cohorts
        // appear as 6/10 each. Naive ceil(0.6n) would fork at this exact
        // multiple of five; calculateParticipantThreshold bumps the floor to 7.
        {
            auto const viewA =
                inspect(0, hashA, {0, 1, 2, 3, 8, 9}, {4, 5, 6, 7});
            auto const viewB =
                inspect(4, hashB, {0, 1, 2, 3}, {4, 5, 6, 7, 8, 9});
            auto const naiveSixty = std::size_t{6};
            auto const participant = calculateParticipantThreshold(10);

            BEAST_EXPECT(viewA.alignedParticipants() == naiveSixty);
            BEAST_EXPECT(viewB.alignedParticipants() == naiveSixty);
            BEAST_EXPECT(viewA.quorumAligned(naiveSixty));
            BEAST_EXPECT(viewB.quorumAligned(naiveSixty));
            BEAST_EXPECT(participant == 7);
            BEAST_EXPECT(!viewA.quorumAligned(participant));
            BEAST_EXPECT(!viewB.quorumAligned(participant));
        }
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
        BEAST_EXPECT(view.containsOriginalMaster(keys[0]));
        BEAST_EXPECT(!view.containsMaster(keys[1]));
        BEAST_EXPECT(!view.containsOriginalMaster(keys[1]));
        BEAST_EXPECT(
            view.orderedOriginalMasterKeys == std::vector<PublicKey>{keys[0]});
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
        BEAST_EXPECT(view.containsOriginalMaster(keys[0]));
        BEAST_EXPECT(view.containsOriginalMaster(keys[1]));
        BEAST_EXPECT(std::is_sorted(
            view.orderedOriginalMasterKeys.begin(),
            view.orderedOriginalMasterKeys.end()));

        source.negativeUNLEnabled = true;
        source.negativeUNL.insert(keys[0]);

        auto const negativeView = buildActiveValidatorView(source, fallback);
        BEAST_EXPECT(!negativeView.fromUNLReport);
        BEAST_EXPECT(negativeView.size() == 1);
        // nUNL shrinks the effective view but NOT the original-UNL denominator.
        BEAST_EXPECT(negativeView.originalViewSize == 2);
        BEAST_EXPECT(!negativeView.containsMaster(keys[0]));
        BEAST_EXPECT(negativeView.containsMaster(keys[1]));
        BEAST_EXPECT(negativeView.containsOriginalMaster(keys[0]));
        BEAST_EXPECT(negativeView.containsOriginalMaster(keys[1]));
        BEAST_EXPECT(negativeView.orderedOriginalMasterKeys.size() == 2);
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
    testActiveValidatorViewCapsNegativeUNL()
    {
        testcase("Active validator view caps NegativeUNL against active set");

        constexpr std::size_t kOriginal = 10;
        constexpr auto kLegalDisable =
            NegativeUNLVote::maxNegativeUNLListed(kOriginal);
        constexpr std::size_t kOverCapDisable = 8;

        std::vector<PublicKey> activeKeys;
        activeKeys.reserve(kOriginal);
        for (std::size_t i = 0; i < kOriginal; ++i)
            activeKeys.push_back(randomKeyPair(KeyType::secp256k1).first);

        auto makeSource = [&](std::size_t disabledCount) {
            ActiveValidatorViewSource source;
            source.sourceLedgerHash = makeHash("active-validator-view-cap");
            source.unlReportMasterKeys.emplace();
            for (auto const& masterKey : activeKeys)
                source.unlReportMasterKeys->insert(masterKey);
            source.negativeUNLEnabled = true;
            for (std::size_t i = 0; i < disabledCount; ++i)
                source.negativeUNL.insert(activeKeys[i]);
            return source;
        };

        ActiveValidatorViewFallback fallback;

        auto const legalView =
            buildActiveValidatorView(makeSource(kLegalDisable), fallback);
        BEAST_EXPECT(legalView.fromUNLReport);
        BEAST_EXPECT(legalView.originalViewSize == kOriginal);
        BEAST_EXPECT(legalView.size() == kOriginal - kLegalDisable);
        for (std::size_t i = 0; i < kLegalDisable; ++i)
            BEAST_EXPECT(!legalView.containsMaster(activeKeys[i]));

        auto const source = makeSource(kOverCapDisable);
        auto const cappedView = buildActiveValidatorView(source, fallback);

        BEAST_EXPECT(cappedView.fromUNLReport);
        BEAST_EXPECT(cappedView.sourceLedgerHash == source.sourceLedgerHash);
        BEAST_EXPECT(cappedView.originalViewSize == kOriginal);
        BEAST_EXPECT(cappedView.size() == kOriginal - kLegalDisable);

        std::vector<PublicKey> cappedDisabled(
            activeKeys.begin(), activeKeys.begin() + kOverCapDisable);
        std::sort(cappedDisabled.begin(), cappedDisabled.end());

        for (std::size_t i = 0; i < cappedDisabled.size(); ++i)
        {
            auto const wasCanonicallyRemoved = i < kLegalDisable;
            if (wasCanonicallyRemoved)
                BEAST_EXPECT(!cappedView.containsMaster(cappedDisabled[i]));
            else
                BEAST_EXPECT(cappedView.containsMaster(cappedDisabled[i]));
        }

        BEAST_EXPECT(
            ConsensusExtensions::entropyGateThresholdForView(2, kOriginal) ==
            2);
        BEAST_EXPECT(
            ConsensusExtensions::entropyGateThresholdForView(
                cappedView.size(), cappedView.originalViewSize) == 6);
        BEAST_EXPECT(
            ConsensusExtensions::selectEntropyTierForView(
                true, 2, cappedView.size(), cappedView.originalViewSize) ==
            entropyTierConsensusFallback);
        BEAST_EXPECT(
            ConsensusExtensions::exportRootAlignmentThreshold(cappedView) == 6);
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
        BEAST_EXPECT(NegativeUNLVote::maxNegativeUNLListed(10) == 3);

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

        // The active-view nUNL cap must preserve the same honest-intersection
        // invariant when the entropy gate takes the effective-view quorum
        // branch. This pins the proof obligation from the walkthrough:
        // for active-disabled d <= ceil(n/4), two gate-sized cohorts inside the
        // effective view n-d still overlap by more than floor(n/5).
        for (std::size_t n = 1; n <= 256; ++n)
        {
            auto const maxDisabled = NegativeUNLVote::maxNegativeUNLListed(n);
            for (std::size_t d = 0; d <= maxDisabled; ++d)
            {
                auto const effective = n - d;
                auto const t = ConsensusExtensions::entropyGateThresholdForView(
                    effective, n);
                BEAST_EXPECT(2 * t > effective + n / 5);
            }
        }
    }

    void
    testThresholdPolicyHelpers()
    {
        testcase("consensus-extension threshold policy helpers");

        // Empty views fail closed at one participant. The raw formulas stay
        // mathematical; sidecar gates use the safe wrappers.
        BEAST_EXPECT(calculateQuorumThreshold(0) == 0);
        BEAST_EXPECT(safeQuorumThreshold(0) == 1);
        BEAST_EXPECT(safeQuorumThreshold(6) == calculateQuorumThreshold(6));
        BEAST_EXPECT(calculateParticipantThreshold(0) == 1);
        BEAST_EXPECT(safeParticipantThreshold(0) == 1);
        BEAST_EXPECT(
            safeParticipantThreshold(10) == calculateParticipantThreshold(10));
        auto const max = std::numeric_limits<std::size_t>::max();
        auto const maxByzantine = max / 5;
        BEAST_EXPECT(detail::floorHalfSum(max, max) == max);
        BEAST_EXPECT(detail::floorHalfSum(max, max - 1) == max - 1);
        BEAST_EXPECT(detail::floorHalfSum(max, 0) == max / 2);
        BEAST_EXPECT(
            calculateQuorumThreshold(max) ==
            max / 100 * 80 + (max % 100 * 80 + 99) / 100);
        BEAST_EXPECT(
            calculateParticipantThreshold(max) ==
            detail::floorHalfSum(max, maxByzantine) + 1);

        // Gate threshold uses effective view for the 80% quorum and original
        // view for the Tier-2 floor, then takes the lower enabled bar.
        BEAST_EXPECT(
            ConsensusExtensions::entropyGateThresholdForView(0, 0) == 1);
        BEAST_EXPECT(
            ConsensusExtensions::entropyGateThresholdForView(6, 6) == 4);
        BEAST_EXPECT(
            ConsensusExtensions::entropyGateThresholdForView(8, 10) == 7);
        BEAST_EXPECT(
            ConsensusExtensions::entropyGateThresholdForView(6, 10) == 5);

        // Tier labels require a ledger-anchored UNLReport view. With one, the
        // ladder is validator_full first, then validator_quorum, then
        // participant_aligned, then fallback.
        BEAST_EXPECT(
            ConsensusExtensions::selectEntropyTierForView(false, 99, 8, 10) ==
            entropyTierConsensusFallback);
        BEAST_EXPECT(
            ConsensusExtensions::selectEntropyTierForView(true, 8, 8, 10) ==
            entropyTierValidatorFull);
        BEAST_EXPECT(
            ConsensusExtensions::selectEntropyTierForView(true, 7, 8, 10) ==
            entropyTierValidatorQuorum);
        BEAST_EXPECT(
            ConsensusExtensions::selectEntropyTierForView(true, 6, 8, 10) ==
            entropyTierConsensusFallback);
        BEAST_EXPECT(
            ConsensusExtensions::selectEntropyTierForView(true, 5, 8, 8) ==
            entropyTierParticipantAligned);
        BEAST_EXPECT(
            ConsensusExtensions::selectEntropyTierForView(true, 4, 8, 8) ==
            entropyTierConsensusFallback);
        BEAST_EXPECT(
            ConsensusExtensions::selectEntropyTierForView(true, 0, 0, 0) ==
            entropyTierConsensusFallback);
    }

    void
    testRuntimeConfigPolicyAccessors()
    {
        testcase("runtime config policy accessors");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        ConsensusExtensions ce{env.app(), activeNoopJournal()};

        BEAST_EXPECT(!ce.testBootstrapFastStartEnabled());
        BEAST_EXPECT(!ce.testSuppressExportSigSetHash());

        ConsensusTestConfig cfg;
        cfg.bootstrapFastStart = true;
        cfg.noExportSigHash = true;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);
        BEAST_EXPECT(ce.testBootstrapFastStartEnabled());
        BEAST_EXPECT(ce.testSuppressExportSigSetHash());

        cfg.bootstrapFastStart = false;
        cfg.noExportSigHash = false;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);
        BEAST_EXPECT(!ce.testBootstrapFastStartEnabled());
        BEAST_EXPECT(!ce.testSuppressExportSigSetHash());
    }

    void
    testOnRoundStartRefreshesFeatureLatches()
    {
        testcase("onRoundStart refreshes extension feature latches");

        //@@start test-round-extension-feature-latches
        using namespace jtx;
        Env enabledEnv{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy | featureExport,
            nullptr};
        Env disabledEnv{
            *this,
            envconfig(validator, ""),
            supported_amendments() - featureConsensusEntropy - featureExport,
            nullptr};

        auto const enabledLedger =
            enabledEnv.app().getLedgerMaster().getClosedLedger();
        auto const disabledLedger =
            disabledEnv.app().getLedgerMaster().getClosedLedger();

        ConsensusExtensions ce{enabledEnv.app(), activeNoopJournal()};
        auto const origin = makeHash("on-round-start-export-latch");
        auto const pk = makeValidatorKeys().front();
        std::uint8_t const sigBytes[] = {1, 2, 3};
        Buffer const sig{sigBytes, sizeof(sigBytes)};

        ce.setRngEnabledThisRound(false);
        ce.setExportEnabledThisRound(false);
        ce.onRoundStart(RCLCxLedger{enabledLedger}, {});
        BEAST_EXPECT(ce.rngEnabled());
        BEAST_EXPECT(ce.exportEnabled());

        BEAST_EXPECT(
            ce.postValidationExportSigCollector().registerOrigin(origin, 10));
        auto admission =
            ce.postValidationExportSigCollector().beginAttributedAdmission(
                origin, ExportSigCollector::Contribution{0, pk, sig}, 10);
        BEAST_EXPECT(
            admission.result == ExportSigCollector::BeginResult::verify);
        BEAST_EXPECT(admission.ticket);
        if (admission.ticket)
            BEAST_EXPECT(
                ce.postValidationExportSigCollector()
                    .admitContribution(std::move(*admission.ticket), true, 10)
                    .result == ExportSigCollector::AdmitResult::accepted);
        BEAST_EXPECT(
            ce.postValidationExportSigCollector().fullUnionSnapshot().size() ==
            1);
        ce.onRoundStart(RCLCxLedger{enabledLedger}, {});
        BEAST_EXPECT(
            ce.postValidationExportSigCollector().fullUnionSnapshot().size() ==
            1);

        ce.setRngEnabledThisRound(true);
        ce.setExportEnabledThisRound(true);
        ce.onRoundStart(RCLCxLedger{disabledLedger}, {});
        BEAST_EXPECT(!ce.rngEnabled());
        BEAST_EXPECT(!ce.exportEnabled());
        BEAST_EXPECT(
            ce.postValidationExportSigCollector().fullUnionSnapshot().empty());
        //@@end test-round-extension-feature-latches
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
        BEAST_EXPECT(tx->getFieldU16(sfEntropyDenominator) == 0);
        BEAST_EXPECT(tx->getFieldVL(sfEntropyContributors).empty());
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
    testTxnOrderingSaltExtendsLegacySalt()
    {
        testcase("transaction ordering salt extends legacy salt");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy,
            nullptr};
        forceNonStandalone(env.app());
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        auto const seq = ledger->seq() + 1;
        auto const txSetHash = makeHash("txn-order-base-set");

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.onRoundStart(RCLCxLedger{ledger}, {});

        ce.setRngEnabledThisRound(false);
        BEAST_EXPECT(ce.txnOrderingSalt(txSetHash, seq) == txSetHash);

        ce.setRngEnabledThisRound(true);
        auto const fallbackDigest = sha512Half(
            HashPrefix::entropyFallback, ledger->info().hash, txSetHash, seq);
        auto const expected = sha512Half(
            HashPrefix::entropyTxnOrder,
            txSetHash,
            fallbackDigest,
            static_cast<std::uint8_t>(entropyTierConsensusFallback),
            static_cast<std::uint16_t>(0),
            static_cast<std::uint16_t>(0));

        auto const salt = ce.txnOrderingSalt(txSetHash, seq);
        BEAST_EXPECT(salt == expected);
        BEAST_EXPECT(salt != txSetHash);
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
        ce.onRoundStart(RCLCxLedger{ledger}, {});
        ce.cacheUNLReport(viewLedger);
        ce.setRngEnabledThisRound(true);
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
        ce.acceptEntropySet(entropySetHash);
        BEAST_EXPECT(
            env.app().getInboundTransactions().getSet(entropySetHash, false));

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
        BEAST_EXPECT(tx->getFieldU16(sfEntropyDenominator) == 1);
        BEAST_EXPECT(tx->getFieldVL(sfEntropyContributors) == Blob{0x01});
        BEAST_EXPECT(tx->getFieldU8(sfEntropyTier) == entropyTierValidatorFull);
    }

    void
    testOnPreBuildCanonicalizesMultiContributorEntropy()
    {
        testcase("onPreBuild canonicalizes multi-contributor entropy");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy,
            nullptr};
        forceNonStandalone(env.app());

        std::vector<std::pair<PublicKey, SecretKey>> validators;
        validators.push_back(randomKeyPair(KeyType::secp256k1));
        validators.push_back(randomKeyPair(KeyType::secp256k1));
        std::sort(
            validators.begin(),
            validators.end(),
            [](auto const& a, auto const& b) {
                return a.first.slice() < b.first.slice();
            });
        std::vector<PublicKey> activeKeys{
            validators[0].first, validators[1].first};
        auto const viewLedger = makeUNLReportLedger(env, activeKeys);
        auto const anchor = env.app().getLedgerMaster().getClosedLedger();
        auto const seq = anchor->info().seq + 1;
        auto const closeTime = NetClock::time_point{NetClock::duration{322}};
        auto const txSetHash = makeHash("multi-contributor-entropy-txset");
        std::vector<uint256> const reveals{
            makeHash("multi-contributor-reveal-a"),
            makeHash("multi-contributor-reveal-b")};

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.onRoundStart(RCLCxLedger{anchor}, {});
        ce.cacheUNLReport(viewLedger);
        ce.setRngEnabledThisRound(true);

        // Deliberately harvest in reverse signing-key order. The accepted
        // entropy bytes must depend on canonical key/reveal order, not packet
        // arrival or SHAMap traversal order.
        for (std::size_t i : {std::size_t{1}, std::size_t{0}})
        {
            harvestCommitReveal(
                ce,
                calcNodeID(validators[i].first),
                validators[i].first,
                validators[i].second,
                txSetHash,
                seq,
                closeTime,
                anchor->info().hash,
                reveals[i]);
        }
        BEAST_EXPECT(ce.hasQuorumOfCommits());
        BEAST_EXPECT(ce.hasMinimumReveals());

        auto const entropySetHash = ce.buildEntropySet(seq);
        ce.acceptEntropySet(entropySetHash);
        CanonicalTXSet txs{makeHash("multi-contributor-entropy-salt")};
        ce.onPreBuild(txs, seq, txSetHash);

        auto const tx = singleCanonicalTx(txs);
        BEAST_EXPECT(tx);
        if (!tx)
            return;

        BEAST_EXPECT(tx->getTxnType() == ttCONSENSUS_ENTROPY);
        BEAST_EXPECT(
            tx->getFieldH256(sfDigest) ==
            expectedEntropy(
                {{validators[0].first, reveals[0]},
                 {validators[1].first, reveals[1]}}));
        BEAST_EXPECT(tx->getFieldU16(sfEntropyCount) == 2);
        BEAST_EXPECT(tx->getFieldU16(sfEntropyDenominator) == 2);
        BEAST_EXPECT(tx->getFieldVL(sfEntropyContributors) == Blob{0x03});
        BEAST_EXPECT(tx->getFieldU8(sfEntropyTier) == entropyTierValidatorFull);
    }

    void
    testOnPreBuildAcceptedEntropySetOverridesLocalFailureFlag()
    {
        testcase(
            "onPreBuild accepted entropy set overrides local failure flag");

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
        auto const closeTime = NetClock::time_point{NetClock::duration{654}};
        auto const txSetHash = makeHash("prebuild-failed-entropy-txset");
        auto const reveal = makeHash("prebuild-failed-entropy-reveal");
        auto const viewLedger =
            makeUNLReportLedger(env, std::vector<PublicKey>{publicKey});
        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.onRoundStart(RCLCxLedger{ledger}, {});
        ce.cacheUNLReport(viewLedger);
        ce.setRngEnabledThisRound(true);
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
        ce.acceptEntropySet(entropySetHash);
        BEAST_EXPECT(
            env.app().getInboundTransactions().getSet(entropySetHash, false));

        ce.setEntropyFailed();

        CanonicalTXSet retriableTxs{makeHash("failed-entropy-prebuild-salt")};
        ce.onPreBuild(retriableTxs, seq, txSetHash);

        auto const tx = singleCanonicalTx(retriableTxs);
        BEAST_EXPECT(tx);
        if (!tx)
            return;

        BEAST_EXPECT(tx->getTxnType() == ttCONSENSUS_ENTROPY);
        BEAST_EXPECT(
            tx->getFieldH256(sfDigest) == expectedEntropy(publicKey, reveal));
        BEAST_EXPECT(tx->getFieldU16(sfEntropyCount) == 1);
        BEAST_EXPECT(tx->getFieldU16(sfEntropyDenominator) == 1);
        BEAST_EXPECT(tx->getFieldVL(sfEntropyContributors) == Blob{0x01});
        BEAST_EXPECT(tx->getFieldU8(sfEntropyTier) == entropyTierValidatorFull);
    }

    void
    testOnPreBuildRejectsForgedEntropyContributorAttribution()
    {
        testcase("onPreBuild rejects forged entropy contributor attribution");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy,
            nullptr};
        forceNonStandalone(env.app());

        auto const first = randomKeyPair(KeyType::secp256k1);
        auto const second = randomKeyPair(KeyType::secp256k1);
        auto const viewLedger =
            makeUNLReportLedger(env, {first.first, second.first});
        auto const anchor = env.app().getLedgerMaster().getClosedLedger();
        auto const seq = anchor->info().seq + 1;
        auto const txSetHash = makeHash("forged-contributor-txset");
        auto const reveal = makeHash("forged-contributor-reveal");

        auto makeRevealSidecar = [&](NodeID const& claimedNode) {
            STObject sidecar(sfGeneric);
            sidecar.setFieldU8(sfSidecarType, sidecarRngReveal);
            sidecar.setFieldU32(sfLedgerSequence, seq);
            sidecar.setAccountID(sfAccount, accountFromNode(claimedNode));
            sidecar.setFieldH256(sfDigest, reveal);
            sidecar.setFieldVL(sfSigningPubKey, first.first.slice());

            Serializer s;
            sidecar.add(s);
            return make_shamapitem(
                sidecar.getHash(HashPrefix::sidecar), s.slice());
        };

        auto map = std::make_shared<SHAMap>(
            SHAMapType::SIDECAR, env.app().getNodeFamily());
        map->setUnbacked();
        map->addItem(
            SHAMapNodeType::tnSIDECAR,
            makeRevealSidecar(calcNodeID(first.first)));
        map->addItem(
            SHAMapNodeType::tnSIDECAR,
            makeRevealSidecar(calcNodeID(second.first)));
        map = map->snapShot(false);

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.onRoundStart(RCLCxLedger{anchor}, {});
        ce.cacheUNLReport(viewLedger);
        ce.setRngEnabledThisRound(true);
        ce.entropySetMap_ = map;
        ce.acceptEntropySet(map->getHash().as_uint256());

        CanonicalTXSet txs{makeHash("forged-contributor-salt")};
        ce.onPreBuild(txs, seq, txSetHash);

        auto const tx = singleCanonicalTx(txs);
        BEAST_EXPECT(tx);
        if (!tx)
            return;

        auto const expectedFallback = sha512Half(
            HashPrefix::entropyFallback, anchor->info().hash, txSetHash, seq);
        BEAST_EXPECT(tx->getTxnType() == ttCONSENSUS_ENTROPY);
        BEAST_EXPECT(tx->getFieldH256(sfDigest) == expectedFallback);
        BEAST_EXPECT(tx->getFieldU16(sfEntropyCount) == 0);
        BEAST_EXPECT(tx->getFieldU16(sfEntropyDenominator) == 0);
        BEAST_EXPECT(tx->getFieldVL(sfEntropyContributors).empty());
        BEAST_EXPECT(
            tx->getFieldU8(sfEntropyTier) == entropyTierConsensusFallback);
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

        std::vector<PublicKey> activeMasterKeys;
        activeMasterKeys.reserve(vals.size());
        for (auto const& [pk, _] : vals)
            activeMasterKeys.push_back(pk);

        // Harvest commit+reveal from `revealers` of the 6 validators, build the
        // agreed entropy set, inject, and return the labelled
        // (tier, count, denominator, contributor mask).
        auto runWith = [&](std::size_t revealers) {
            ConsensusExtensions ce{env.app(), activeNoopJournal()};
            ce.cacheUNLReport(viewLedger);
            ce.setRngEnabledThisRound(true);
            for (std::size_t i = 0; i < revealers; ++i)
            {
                auto const nodeId = calcNodeID(vals[i].first);
                auto const reveal =
                    sha512Half(vals[i].first, makeHash("tier2-reveal"));
                harvestCommitReveal(
                    ce,
                    nodeId,
                    vals[i].first,
                    vals[i].second,
                    txSetHash,
                    seq,
                    closeTime,
                    prevLedger,
                    reveal);
            }
            auto const entropySetHash = ce.buildEntropySet(seq);
            ce.acceptEntropySet(entropySetHash);
            CanonicalTXSet txs{makeHash("tier2-salt")};
            ce.onPreBuild(txs, seq, txSetHash);
            auto const tx = singleCanonicalTx(txs);
            std::tuple<int, std::uint16_t, std::uint16_t, Blob> out{
                -1, 0, 0, Blob{}};
            if (tx)
                out = {
                    tx->getFieldU8(sfEntropyTier),
                    tx->getFieldU16(sfEntropyCount),
                    tx->getFieldU16(sfEntropyDenominator),
                    tx->getFieldVL(sfEntropyContributors)};
            return out;
        };

        // 5 of 6 aligned -> validator_quorum (count >= quorum 5).
        auto const q = runWith(5);
        BEAST_EXPECT(std::get<0>(q) == entropyTierValidatorQuorum);
        BEAST_EXPECT(std::get<1>(q) == 5);
        BEAST_EXPECT(std::get<2>(q) == kValidators);
        BEAST_EXPECT(
            std::get<3>(q) ==
            expectedContributorMask(
                activeMasterKeys,
                {calcNodeID(vals[0].first),
                 calcNodeID(vals[1].first),
                 calcNodeID(vals[2].first),
                 calcNodeID(vals[3].first),
                 calcNodeID(vals[4].first)}));

        // 4 of 6 aligned -> participant_aligned (count >= tier2 4, < quorum 5).
        auto const p = runWith(4);
        BEAST_EXPECT(std::get<0>(p) == entropyTierParticipantAligned);
        BEAST_EXPECT(std::get<1>(p) == 4);
        BEAST_EXPECT(std::get<2>(p) == kValidators);
        BEAST_EXPECT(
            std::get<3>(p) ==
            expectedContributorMask(
                activeMasterKeys,
                {calcNodeID(vals[0].first),
                 calcNodeID(vals[1].first),
                 calcNodeID(vals[2].first),
                 calcNodeID(vals[3].first)}));

        // 3 of 6 aligned -> below the tier-2 floor -> consensus_fallback.
        auto const f = runWith(3);
        BEAST_EXPECT(std::get<0>(f) == entropyTierConsensusFallback);
        BEAST_EXPECT(std::get<1>(f) == 0);
        BEAST_EXPECT(std::get<2>(f) == 0);
        BEAST_EXPECT(std::get<3>(f).empty());
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
        std::vector<PublicKey> effectiveMasterKeys(
            activeKeys.begin() + kDisabled, activeKeys.end());

        auto runWith = [&](std::size_t revealers) {
            ConsensusExtensions ce{env.app(), activeNoopJournal()};
            ce.cacheUNLReport(viewLedger);
            ce.setRngEnabledThisRound(true);
            for (std::size_t i = 0; i < revealers; ++i)
            {
                auto const idx = i + kDisabled;  // skip disabled validator
                auto const nodeId = calcNodeID(vals[idx].first);
                auto const reveal =
                    sha512Half(vals[idx].first, makeHash("tier2-nunl-reveal"));
                harvestCommitReveal(
                    ce,
                    nodeId,
                    vals[idx].first,
                    vals[idx].second,
                    txSetHash,
                    seq,
                    closeTime,
                    prevLedger,
                    reveal);
            }
            auto const entropySetHash = ce.buildEntropySet(seq);
            ce.acceptEntropySet(entropySetHash);
            CanonicalTXSet txs{makeHash("tier2-nunl-salt")};
            ce.onPreBuild(txs, seq, txSetHash);
            auto const tx = singleCanonicalTx(txs);
            std::tuple<int, std::uint16_t, std::uint16_t, Blob> out{
                -1, 0, 0, Blob{}};
            if (tx)
                out = {
                    tx->getFieldU8(sfEntropyTier),
                    tx->getFieldU16(sfEntropyCount),
                    tx->getFieldU16(sfEntropyDenominator),
                    tx->getFieldVL(sfEntropyContributors)};
            return out;
        };

        auto const q = runWith(16);
        BEAST_EXPECT(std::get<0>(q) == entropyTierValidatorQuorum);
        BEAST_EXPECT(std::get<1>(q) == 16);
        BEAST_EXPECT(std::get<2>(q) == kOriginal - kDisabled);
        {
            std::vector<NodeID> expectedNodes;
            expectedNodes.reserve(16);
            for (std::size_t i = 0; i < 16; ++i)
                expectedNodes.push_back(calcNodeID(vals[i + kDisabled].first));
            BEAST_EXPECT(
                std::get<3>(q) ==
                expectedContributorMask(effectiveMasterKeys, expectedNodes));
        }

        auto const p = runWith(15);
        BEAST_EXPECT(std::get<0>(p) == entropyTierParticipantAligned);
        BEAST_EXPECT(std::get<1>(p) == 15);
        BEAST_EXPECT(std::get<2>(p) == kOriginal - kDisabled);
        {
            std::vector<NodeID> expectedNodes;
            expectedNodes.reserve(15);
            for (std::size_t i = 0; i < 15; ++i)
                expectedNodes.push_back(calcNodeID(vals[i + kDisabled].first));
            BEAST_EXPECT(
                std::get<3>(p) ==
                expectedContributorMask(effectiveMasterKeys, expectedNodes));
        }

        auto const f = runWith(12);
        BEAST_EXPECT(std::get<0>(f) == entropyTierConsensusFallback);
        BEAST_EXPECT(std::get<1>(f) == 0);
        BEAST_EXPECT(std::get<2>(f) == 0);
        BEAST_EXPECT(std::get<3>(f).empty());
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

        auto const edKeys = randomKeyPair(KeyType::ed25519);
        BEAST_EXPECT(!ConsensusExtensions::verifyProof(
            blob, edKeys.first, *position.myCommitment, true));

        Blob malformed{1, 2, 3};
        BEAST_EXPECT(!ConsensusExtensions::deserializeProof(malformed));
        BEAST_EXPECT(!ConsensusExtensions::verifyProof(
            malformed, publicKey, *position.myCommitment, true));
    }

    void
    testProposalPrecheckUsesExportShareRelayLimits()
    {
        testcase("proposal precheck uses serialized ExportShare limits");

        using enum detail::ProposalPrecheckResult;

        auto const signer = randomKeyPair(KeyType::secp256k1);
        auto const owner = calcAccountID(signer.first);
        auto const destination =
            calcAccountID(randomKeyPair(KeyType::secp256k1).first);
        auto const releaseTarget =
            makeSTTx(makeExportedPayment(owner, destination));
        auto signature = ExportResultBuilder::signExportedTxn(
            releaseTarget, signer.first, signer.second);
        ExportShare const share{
            ExportShare::currentVersion,
            owner,
            makeHash("precheck-export-origin"),
            10,
            makeHash("precheck-export-origin-ledger"),
            3,
            signer.first,
            std::move(signature)};
        auto const serialized = share.serialize();
        std::string const frame{
            reinterpret_cast<char const*>(serialized.data()),
            serialized.size()};

        BEAST_EXPECT(frame.size() > 137);
        BEAST_EXPECT(
            frame.size() <= ExportLimits::maxSerializedExportShareBytes);
        BEAST_EXPECT(ExportShare::parse(makeSlice(frame)));

        auto const makeProposal = [&](std::vector<std::string> const& frames) {
            protocol::TMProposeSet set;
            auto const previous = makeHash("precheck-previous-ledger");
            set.set_previousledger(previous.data(), previous.size());
            for (auto const& bytes : frames)
                set.add_exportsignatures(bytes);

            ExtendedPosition position{makeHash("precheck-position")};
            position.exportSignaturesHash =
                proposalExportSignaturesHash(frames);
            Serializer positionData;
            position.add(positionData);
            set.set_currenttxhash(positionData.data(), positionData.size());
            return set;
        };

        std::vector<std::string> atLimit(
            ExportLimits::maxExportSharesPerRelay, frame);
        auto const accepted = makeProposal(atLimit);
        BEAST_EXPECT(
            detail::checkProposalExtensions(accepted, true, true).result == ok);

        auto overLimit = atLimit;
        overLimit.push_back(frame);
        auto const rejectedCount = makeProposal(overLimit);
        BEAST_EXPECT(
            detail::checkProposalExtensions(rejectedCount, true, true).result ==
            tooManyExportSignatures);

        std::string oversized = frame;
        oversized.resize(ExportLimits::maxSerializedExportShareBytes + 1, '\0');
        auto const rejectedSize =
            makeProposal(std::vector<std::string>{std::move(oversized)});
        BEAST_EXPECT(
            detail::checkProposalExtensions(rejectedSize, true, true).result ==
            oversizedExportSignature);
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
        BEAST_EXPECT(ce.pendingRevealCount() == 1);
        BEAST_EXPECT(ce.hasQuorumOfCommits());

        ce.harvestRngData(
            nodeId, publicKey, earlyReveal, 3, closeTime, prevLedger, Slice{});
        BEAST_EXPECT(ce.pendingRevealCount() == 1);

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
        // The later proofless commitment was ignored, so the original proofed
        // commit remains authoritative and the mismatched reveal cannot replace
        // the already-accepted reveal.
        BEAST_EXPECT(ce.pendingRevealCount() == 1);
    }

    void
    testRngManifestArrivalDoesNotRetargetProofedCommit()
    {
        testcase("RNG manifest arrival does not retarget proofed commit");

        using namespace jtx;
        auto const masterSeed = randomSeed();
        Env env{
            *this,
            envconfig(validator, toBase58(masterSeed)),
            supported_amendments(),
            nullptr};
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        auto const masterSecret =
            generateSecretKey(KeyType::secp256k1, masterSeed);
        auto const signingSecret2 =
            generateSecretKey(KeyType::secp256k1, randomSeed());
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;
        auto const& masterKey = valKeys.keys->masterPublicKey;
        auto const& signingKey1 = valKeys.keys->publicKey;
        auto const& signingSecret1 = valKeys.keys->secretKey;
        auto const signingKey2 =
            derivePublicKey(KeyType::secp256k1, signingSecret2);
        auto const nodeId = valKeys.nodeID;
        BEAST_EXPECT(masterKey == signingKey1);

        ConsensusExtensions manifestFirst{env.app(), activeNoopJournal()};
        ConsensusExtensions proposalFirst{env.app(), activeNoopJournal()};
        manifestFirst.cacheUNLReport(ledger);
        proposalFirst.cacheUNLReport(ledger);

        auto const seq = ledger->seq() + 1;
        auto const closeTime = NetClock::time_point{NetClock::duration{655}};
        auto const txSetHash = makeHash("manifest-arrival-txset");
        auto const reveal = makeHash("manifest-arrival-reveal");
        auto const commitment = sha512Half(reveal, signingKey1, seq);
        for (auto* ce : {&manifestFirst, &proposalFirst})
            harvestCommitReveal(
                *ce,
                nodeId,
                signingKey1,
                signingSecret1,
                txSetHash,
                seq,
                closeTime,
                ledger->info().hash,
                reveal);

        ExtendedPosition rotated{txSetHash};
        rotated.myCommitment = commitment;
        auto const rotatedSig = signPosition(
            signingKey2,
            signingSecret2,
            rotated,
            2,
            closeTime,
            ledger->info().hash);

        // Before the manifest arrives, the rotated key is not trusted and the
        // proposal is ignored. Installing the manifest later does not replay
        // the packet.
        proposalFirst.harvestRngData(
            nodeId,
            signingKey2,
            rotated,
            2,
            closeTime,
            ledger->info().hash,
            Slice(rotatedSig.data(), rotatedSig.size()));
        BEAST_EXPECT(
            env.app().validatorManifests().applyManifest(
                makeValidatorManifest(masterSecret, signingSecret2, 1)) ==
            ManifestDisposition::accepted);

        // The same packet is trusted when the manifest arrives first, but the
        // proofed K1 commit must keep K1 as its per-round signing key.
        manifestFirst.harvestRngData(
            nodeId,
            signingKey2,
            rotated,
            2,
            closeTime,
            ledger->info().hash,
            Slice(rotatedSig.data(), rotatedSig.size()));

        BEAST_EXPECT(
            manifestFirst.pendingCommits_ == proposalFirst.pendingCommits_);
        BEAST_EXPECT(
            manifestFirst.pendingReveals_ == proposalFirst.pendingReveals_);
        BEAST_EXPECT(manifestFirst.nodeIdToKey_.at(nodeId) == signingKey1);
        BEAST_EXPECT(proposalFirst.nodeIdToKey_.at(nodeId) == signingKey1);
        BEAST_EXPECT(
            manifestFirst.buildEntropySet(seq) ==
            proposalFirst.buildEntropySet(seq));
    }

    void
    testRngManifestRotationDropsPinnedContributor()
    {
        testcase("RNG manifest rotation drops pinned contributor");

        using namespace jtx;
        auto const masterSeed = randomSeed();
        Env env{
            *this,
            envconfig(validator, toBase58(masterSeed)),
            supported_amendments(),
            nullptr};
        auto const anchor = env.app().getLedgerMaster().getClosedLedger();
        auto const masterSecret =
            generateSecretKey(KeyType::secp256k1, masterSeed);
        auto const masterKey =
            derivePublicKey(KeyType::secp256k1, masterSecret);
        auto const signingSecret1 =
            generateSecretKey(KeyType::secp256k1, randomSeed());
        auto const signingSecret2 =
            generateSecretKey(KeyType::secp256k1, randomSeed());
        auto const signingKey1 =
            derivePublicKey(KeyType::secp256k1, signingSecret1);
        auto const signingKey2 =
            derivePublicKey(KeyType::secp256k1, signingSecret2);
        auto const nodeId = calcNodeID(masterKey);

        BEAST_EXPECT(
            env.app().validatorManifests().applyManifest(
                makeValidatorManifest(masterSecret, signingSecret1, 1)) ==
            ManifestDisposition::accepted);

        auto const viewLedger =
            makeUNLReportLedger(env, std::vector<PublicKey>{masterKey});
        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.cacheUNLReport(viewLedger);

        auto const seq = anchor->info().seq + 1;
        auto const closeTime = NetClock::time_point{NetClock::duration{656}};
        auto const txSetHash = makeHash("manifest-rotation-txset");
        auto const reveal = makeHash("manifest-rotation-reveal");
        auto const commitment = sha512Half(reveal, signingKey1, seq);

        ExtendedPosition commitPos{txSetHash};
        commitPos.myCommitment = commitment;
        auto const commitSig = signPosition(
            signingKey1,
            signingSecret1,
            commitPos,
            0,
            closeTime,
            anchor->info().hash);
        ce.harvestRngData(
            nodeId,
            signingKey1,
            commitPos,
            0,
            closeTime,
            anchor->info().hash,
            Slice(commitSig.data(), commitSig.size()));
        BEAST_EXPECT(ce.pendingCommitCount() == 1);
        BEAST_EXPECT(ce.hasQuorumOfCommits());

        BEAST_EXPECT(
            env.app().validatorManifests().applyManifest(
                makeValidatorManifest(masterSecret, signingSecret2, 2)) ==
            ManifestDisposition::accepted);

        ExtendedPosition revealPos{txSetHash};
        revealPos.myReveal = reveal;
        auto const retiredKeySig = signPosition(
            signingKey1,
            signingSecret1,
            revealPos,
            1,
            closeTime,
            anchor->info().hash);
        ce.harvestRngData(
            nodeId,
            signingKey1,
            revealPos,
            1,
            closeTime,
            anchor->info().hash,
            Slice(retiredKeySig.data(), retiredKeySig.size()));

        auto const substitutedKeySig = signPosition(
            signingKey2,
            signingSecret2,
            revealPos,
            2,
            closeTime,
            anchor->info().hash);
        ce.harvestRngData(
            nodeId,
            signingKey2,
            revealPos,
            2,
            closeTime,
            anchor->info().hash,
            Slice(substitutedKeySig.data(), substitutedKeySig.size()));

        BEAST_EXPECT(ce.pendingRevealCount() == 0);
        BEAST_EXPECT(!ce.hasMinimumReveals());
        auto const entropyHash = ce.buildEntropySet(seq);
        auto const entropySet =
            env.app().getInboundTransactions().getSet(entropyHash, false);
        BEAST_EXPECT(entropySet);
        BEAST_EXPECT(entropySet && entropySet->getHash().isZero());
    }

    void
    testExportCollectorBuildsAttributedUnion()
    {
        testcase("Export collector builds attributed union");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        auto& collector = ce.postValidationExportSigCollector();
        auto const origin = makeHash("attributed-origin");
        auto const signerA = randomKeyPair(KeyType::secp256k1).first;
        auto const signerB = randomKeyPair(KeyType::secp256k1).first;
        std::uint8_t const signatureABytes[] = {1, 2};
        std::uint8_t const signatureBBytes[] = {3, 4};
        Buffer const signatureA{signatureABytes, sizeof(signatureABytes)};
        Buffer const signatureB{signatureBBytes, sizeof(signatureBBytes)};

        BEAST_EXPECT(collector.registerOrigin(origin, 10));
        auto admit = [&](ExportSigCollector::Position position,
                         PublicKey const& key,
                         Buffer const& signature) {
            auto admission = collector.beginAttributedAdmission(
                origin,
                ExportSigCollector::Contribution{position, key, signature},
                10);
            BEAST_EXPECT(
                admission.result == ExportSigCollector::BeginResult::verify);
            BEAST_EXPECT(admission.ticket);
            if (!admission.ticket)
                return;
            BEAST_EXPECT(
                collector
                    .admitContribution(std::move(*admission.ticket), true, 10)
                    .result == ExportSigCollector::AdmitResult::accepted);
        };
        admit(0, signerA, signatureA);
        admit(2, signerB, signatureB);

        auto const snapshot = collector.fullUnionSnapshot();
        auto const found = snapshot.find(origin);
        BEAST_EXPECT(found != snapshot.end());
        if (found != snapshot.end())
        {
            BEAST_EXPECT(found->second.size() == 2);
            BEAST_EXPECT(found->second[0].position == 0);
            BEAST_EXPECT(found->second[0].signingKey == signerA);
            BEAST_EXPECT(found->second[0].signature == signatureA);
            BEAST_EXPECT(found->second[1].position == 2);
            BEAST_EXPECT(found->second[1].signingKey == signerB);
            BEAST_EXPECT(found->second[1].signature == signatureB);
        }
    }

    void
    testExportSidecarCandidateDeadline()
    {
        testcase("Export sidecar candidate deadline is inclusive");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureExport,
            nullptr};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        env.close();

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        BEAST_EXPECT(parent);
        if (!parent)
            return;
        auto validated = std::make_shared<Ledger>(
            *parent, env.app().timeKeeper().closeTime());
        auto const deadline = validated->info().seq;
        auto const origin = makeHash("export-sidecar-deadline-origin");
        auto latch =
            std::make_shared<SLE>(keylet::exportLatch(alice.id(), origin));
        latch->setAccountID(sfAccount, alice.id());
        latch->setFieldU32(sfTicketSequence, 1);
        latch->setFieldH256(sfTransactionHash, origin);
        latch->setFieldH256(sfDigest, makeHash("export-sidecar-intent"));
        latch->setFieldU32(sfLedgerSequence, deadline);
        latch->setFieldH256(
            sfExportCommitteeHash, validated->info().parentHash);
        latch->setFieldU32(sfLastLedgerSequence, deadline);

        Sandbox sandbox{validated.get(), tapNONE};
        BEAST_EXPECT(isTesSuccess(ExportLedgerOps::insertPendingExportLatch(
            sandbox, sandbox, latch, env.journal)));
        sandbox.apply(*validated);
        validated->updateSkipList();
        validated->setAccepted(
            validated->info().closeTime,
            validated->info().closeTimeResolution,
            true);
        env.app().getLedgerMaster().setFullLedger(validated, false, false);
        auto const currentValidated =
            env.app().getLedgerMaster().getValidatedLedger();
        BEAST_EXPECT(
            currentValidated && currentValidated->info().seq == deadline);
        if (!currentValidated || currentValidated->info().seq != deadline)
            return;

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        auto& collector = ce.postValidationExportSigCollector();
        auto const signer = randomKeyPair(KeyType::secp256k1).first;
        std::uint8_t const signatureBytes[] = {1, 2, 3};
        Buffer const signature{signatureBytes, sizeof(signatureBytes)};
        BEAST_EXPECT(collector.registerOrigin(origin, deadline));
        auto admission = collector.beginAttributedAdmission(
            origin,
            ExportSigCollector::Contribution{0, signer, signature},
            deadline);
        BEAST_EXPECT(admission.ticket);
        if (!admission.ticket)
            return;
        BEAST_EXPECT(
            collector
                .admitContribution(std::move(*admission.ticket), true, deadline)
                .result == ExportSigCollector::AdmitResult::accepted);

        auto const leafCount = [&](uint256 const& hash) {
            auto const map =
                env.app().getInboundTransactions().getSet(hash, false);
            BEAST_EXPECT(map);
            std::size_t count = 0;
            if (map)
                map->visitLeaves([&](auto const&) { ++count; });
            return count;
        };

        // The validated view remains D in both cases. Candidate D is the
        // inclusive final publication opportunity; candidate D+1 is expired.
        ce.buildingLedgerSeq_ = deadline;
        BEAST_EXPECT(ce.hasEligiblePendingExports());
        BEAST_EXPECT(ce.hasPendingExportSigs());
        BEAST_EXPECT(leafCount(ce.buildExportSigSet(deadline)) == 1);

        ce.buildingLedgerSeq_ = deadline + 1;
        BEAST_EXPECT(!ce.hasEligiblePendingExports());
        BEAST_EXPECT(!ce.hasPendingExportSigs());
        BEAST_EXPECT(leafCount(ce.buildExportSigSet(deadline + 1)) == 0);
    }

    void
    testTransactionAcquireRejectsSidecarWireNodes()
    {
        testcase("Transaction acquire rejects sidecar wire nodes");

        jtx::Env env{*this};
        auto const publicKey = makeValidatorKeys().front();

        STObject sidecar(sfGeneric);
        sidecar.setFieldU8(sfSidecarType, sidecarExportSig);
        sidecar.setFieldH256(sfTransactionHash, makeHash("export-tx"));
        sidecar.setFieldVL(sfSigningPubKey, publicKey.slice());
        sidecar.setFieldVL(sfTxnSignature, Slice("sig", 3));

        Serializer itemSer;
        sidecar.add(itemSer);
        auto item = make_shamapitem(
            sidecar.getHash(HashPrefix::sidecar), itemSer.slice());
        SHAMapSidecarLeafNode leaf{std::move(item), 0};

        Serializer wire;
        leaf.serializeForWire(wire);

        TransactionAcquire acquire{
            env.app(),
            leaf.getHash().as_uint256(),
            make_DummyPeerSet(env.app())};
        auto const result = acquire.takeNodes(
            {{SHAMapNodeID(), wire.slice()}}, std::shared_ptr<Peer>{});

        BEAST_EXPECT(result.isInvalid());
    }

    void
    testAcquiredSetsRejectConsensusExtensionPseudos()
    {
        testcase("Network-acquired sets cannot contain CE pseudo txs");

        //@@start test-acquired-ce-pseudo-reject
        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};

        std::size_t callbacks = 0;
        bool lastFromAcquire = false;
        auto inbound = make_InboundTransactions(
            env.app(),
            env.app().getCollectorManager().collector(),
            [&](std::shared_ptr<SHAMap> const&, bool fromAcquire) {
                ++callbacks;
                lastFromAcquire = fromAcquire;
            });

        auto const seq = env.closed()->seq() + 1;
        auto const expectRejectedFromAcquire =
            [&](std::shared_ptr<STTx const> const& pseudo) {
                auto const set = makeRCLTxSet(env.app(), {pseudo});
                auto const setHash = set.id();
                auto const beforeCallbacks = callbacks;

                // A proposal only commits to a candidate tx-set hash. Once
                // the set is acquired, CE-local pseudos must still be
                // unvotable network ingress material, not merely DisputedTxs
                // that an honest minority votes out.
                inbound->giveSet(setHash, set.map_, true);
                BEAST_EXPECT(callbacks == beforeCallbacks);
                BEAST_EXPECT(!inbound->getSet(setHash, false));

                // Local post-agreement CE materialization still uses the same
                // cache. The acquired-set rejection must not break that
                // local-only path.
                inbound->giveSet(setHash, set.map_, false);
                BEAST_EXPECT(callbacks == beforeCallbacks + 1);
                BEAST_EXPECT(!lastFromAcquire);
                auto const localSet = inbound->getSet(setHash, false);
                BEAST_EXPECT(localSet);
                if (localSet)
                    BEAST_EXPECT(localSet->getHash().as_uint256() == setHash);
            };

        expectRejectedFromAcquire(
            makeConsensusEntropyTx(seq, makeHash("hostile-peer-entropy"), 4));
        expectRejectedFromAcquire(makeExportSignaturesTx(
            seq, makeHash("hostile-peer-export-signatures")));
        //@@end test-acquired-ce-pseudo-reject
    }

    void
    testAgreedExportWitnessBuildsContributorBitmap()
    {
        testcase(
            "agreed Export witness materialization is insertion-order "
            "independent");

        using namespace jtx;
        auto const signerA = randomKeyPair(KeyType::secp256k1);
        auto const signerB = randomKeyPair(KeyType::secp256k1);
        auto const signerC = randomKeyPair(KeyType::secp256k1);
        auto const dst = calcAccountID(randomKeyPair(KeyType::secp256k1).first);
        auto const releaseTarget =
            makeSTTx(makeExportedPayment(calcAccountID(signerA.first), dst));
        auto const origin = makeHash("agreed-export-origin");
        Blob const contributors{0x07};
        constexpr std::size_t committeeSize = 3;
        auto const threshold = ExportLimits::committeeQuorumThreshold(3);
        BEAST_EXPECT(threshold == 3);

        struct Contribution
        {
            std::uint32_t position;
            PublicKey signingKey;
            Buffer signature;
        };

        std::vector<Contribution> const contributions = {
            {0,
             signerA.first,
             ExportResultBuilder::signExportedTxn(
                 releaseTarget, signerA.first, signerA.second)},
            {1,
             signerB.first,
             ExportResultBuilder::signExportedTxn(
                 releaseTarget, signerB.first, signerB.second)},
            {2,
             signerC.first,
             ExportResultBuilder::signExportedTxn(
                 releaseTarget, signerC.first, signerC.second)}};

        struct MaterializedWitness
        {
            uint256 root;
            ExportResultBuilder::PositionedSignatureSnapshot signatures;
            Blob contributors;
            std::vector<std::uint8_t> serialized;
            uint256 txid;
        };

        auto const materialize =
            [&](std::vector<std::size_t> const& insertionOrder)
            -> std::optional<MaterializedWitness> {
            Env env{
                *this,
                envconfig(validator, ""),
                supported_amendments(),
                nullptr};
            ConsensusExtensions ce{env.app(), activeNoopJournal()};

            auto map = std::make_shared<SHAMap>(
                SHAMapType::SIDECAR, env.app().getNodeFamily());
            map->setUnbacked();
            for (auto const index : insertionOrder)
            {
                auto const& contribution = contributions[index];
                STObject sidecar(sfGeneric);
                sidecar.setFieldU8(sfSidecarType, sidecarExportSig);
                sidecar.setFieldH256(sfTransactionHash, origin);
                sidecar.setFieldU32(sfTransactionIndex, contribution.position);
                sidecar.setFieldVL(
                    sfSigningPubKey, contribution.signingKey.slice());
                sidecar.setFieldVL(
                    sfTxnSignature,
                    Slice{
                        contribution.signature.data(),
                        contribution.signature.size()});
                Serializer serialized;
                sidecar.add(serialized);
                BEAST_EXPECT(map->addItem(
                    SHAMapNodeType::tnSIDECAR,
                    make_shamapitem(
                        sidecar.getHash(HashPrefix::sidecar),
                        serialized.slice())));
            }
            map = map->snapShot(false);

            auto const acceptedHash = map->getHash().as_uint256();
            env.app().getInboundTransactions().giveSet(
                acceptedHash, map, false);
            BEAST_EXPECT(!ce.agreedExportWitness(
                releaseTarget, origin, committeeSize, threshold));
            ce.acceptExportSigSet(acceptedHash);

            auto const material = ce.agreedExportWitness(
                releaseTarget, origin, committeeSize, threshold);
            BEAST_EXPECT(material);
            if (!material)
                return std::nullopt;

            auto const witness = ExportResultBuilder::buildSignatureWitness(
                origin, releaseTarget, material->signatures, committeeSize, 20);
            BEAST_EXPECT(!witness.isFieldPresent(sfSigners));
            BEAST_EXPECT(
                witness.getFieldVL(sfExportContributors) == contributors);
            auto const& assembled =
                witness.peekAtField(sfExportedTxn).downcast<STObject>();
            BEAST_EXPECT(!assembled.isFieldPresent(sfSigners));
            BEAST_EXPECT(
                witness.getFieldArray(sfExportSigners).size() ==
                contributions.size());

            Serializer serialized;
            witness.add(serialized);
            return MaterializedWitness{
                acceptedHash,
                material->signatures,
                witness.getFieldVL(sfExportContributors),
                serialized.peekData(),
                witness.getTransactionID()};
        };

        auto const forward = materialize({0, 1, 2});
        auto const reverse = materialize({2, 1, 0});
        BEAST_EXPECT(forward && reverse);
        if (!forward || !reverse)
            return;

        auto const expectMapping =
            [&](ExportResultBuilder::PositionedSignatureSnapshot const&
                    signatures) {
                BEAST_EXPECT(signatures.size() == contributions.size());
                for (auto const& expected : contributions)
                {
                    auto const found = signatures.find(expected.position);
                    BEAST_EXPECT(found != signatures.end());
                    if (found != signatures.end())
                    {
                        BEAST_EXPECT(
                            found->second.signingKey == expected.signingKey);
                        BEAST_EXPECT(
                            found->second.signature == expected.signature);
                    }
                }
            };
        expectMapping(forward->signatures);
        expectMapping(reverse->signatures);

        BEAST_EXPECT(forward->root == reverse->root);
        BEAST_EXPECT(forward->contributors == contributors);
        BEAST_EXPECT(reverse->contributors == contributors);
        BEAST_EXPECT(forward->serialized == reverse->serialized);
        BEAST_EXPECT(forward->txid == reverse->txid);
    }

    void
    testRngSidecarBuildsLocalSnapshots()
    {
        testcase("RNG sidecar builds local snapshots");

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
        BEAST_EXPECT(
            env.app().getInboundTransactions().getSet(commitSetHash, false));
        BEAST_EXPECT(
            env.app().getInboundTransactions().getSet(revealSetHash, false));

        source.setRngEnabledThisRound(true);
        source.recordParticipantDiagnostics(
            ConsensusMode::proposing, std::vector<NodeID>{nodeId});
        BEAST_EXPECT(source.observedParticipantCount() == 1);
        BEAST_EXPECT(source.observedParticipantsHash());
        BEAST_EXPECT(!source.observedParticipantsBitmapBin().empty());

        ExtendedPosition diagnosticPos{txSetHash};
        source.attachParticipantDiagnostics(diagnosticPos);
        BEAST_EXPECT(diagnosticPos.observedParticipantsHash);
    }

    void
    testOnPreBuildInjectsStandaloneEntropy()
    {
        testcase("onPreBuild injects standalone entropy pseudo-tx");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.setRngEnabledThisRound(true);
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
        BEAST_EXPECT(tx->getFieldU16(sfEntropyDenominator) == 20);
        BEAST_EXPECT(
            tx->getFieldVL(sfEntropyContributors) ==
            standaloneContributorMask(20, 20));
        BEAST_EXPECT(tx->getFieldU8(sfEntropyTier) == entropyTierValidatorFull);

        // Live materialization is reconstructive: a second call removes the
        // prior synthetic entry and derives the same canonical entry again.
        ce.onPreBuild(retriableTxs, seq, makeHash("standalone-txset"));
        BEAST_EXPECT(
            std::distance(retriableTxs.begin(), retriableTxs.end()) == 1);
    }

    void
    testOnPreBuildStripsSuppliedExtensionPseudos()
    {
        testcase("onPreBuild strips supplied extension pseudo-txs");

        using namespace jtx;
        Env env{
            *this, envconfig(validator, ""), supported_amendments(), nullptr};
        auto const seq = env.closed()->seq() + 1;

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.setRngEnabledThisRound(true);
        CanonicalTXSet txs{makeHash("supplied-extension-pseudos-salt")};
        auto const suppliedEntropy =
            makeConsensusEntropyTx(seq, makeHash("supplied-digest"), 7);
        auto const suppliedEntropyID = suppliedEntropy->getTransactionID();
        auto const suppliedWitness =
            makeExportSignaturesTx(seq, makeHash("supplied-export-origin"));
        auto const suppliedWitnessID = suppliedWitness->getTransactionID();
        txs.insert(suppliedEntropy);
        txs.insert(suppliedWitness);

        ce.onPreBuild(txs, seq, makeHash("supplied-extension-pseudos-txset"));

        BEAST_EXPECT(std::distance(txs.begin(), txs.end()) == 1);
        auto const derived = singleCanonicalTx(txs);
        BEAST_EXPECT(derived);
        if (derived)
        {
            BEAST_EXPECT(derived->getTxnType() == ttCONSENSUS_ENTROPY);
            BEAST_EXPECT(derived->getTransactionID() != suppliedEntropyID);
            BEAST_EXPECT(derived->getTransactionID() != suppliedWitnessID);
            BEAST_EXPECT(
                derived->getFieldH256(sfDigest) ==
                sha512Half(std::string("standalone-entropy"), seq));
        }
    }

    void
    testLiveBuildSetStripsOnlyExtensionPseudos()
    {
        testcase("live build set strips only extension pseudo-txs");

        using namespace jtx;
        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureConsensusEntropy,
            nullptr};
        auto const seq = env.closed()->seq() + 1;
        auto const validatorKey = makeValidatorKeys().front();

        auto const fee = std::make_shared<STTx const>(ttFEE, [&](auto& obj) {
            obj.setAccountID(sfAccount, AccountID{});
            obj.setFieldU32(sfLedgerSequence, seq);
        });
        auto const amendment =
            std::make_shared<STTx const>(ttAMENDMENT, [&](auto& obj) {
                obj.setAccountID(sfAccount, AccountID{});
                obj.setFieldU32(sfLedgerSequence, seq);
                obj.setFieldH256(sfAmendment, makeHash("legacy-amendment"));
            });
        auto const negativeUNL =
            std::make_shared<STTx const>(ttUNL_MODIFY, [&](auto& obj) {
                obj.setAccountID(sfAccount, AccountID{});
                obj.setFieldU8(sfUNLModifyDisabling, 1);
                obj.setFieldU32(sfLedgerSequence, seq);
                obj.setFieldVL(sfUNLModifyValidator, validatorKey);
            });
        auto const suppliedEntropy =
            makeConsensusEntropyTx(seq, makeHash("supplied-digest"), 7);
        auto const suppliedWitness =
            makeExportSignaturesTx(seq, makeHash("supplied-export-origin"));

        auto const agreed = makeRCLTxSet(
            env.app(),
            {fee, amendment, negativeUNL, suppliedEntropy, suppliedWitness});
        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        auto const build = ce.makeLiveBuildTxSet(agreed);

        BEAST_EXPECT(build.suppliedEntropy == 1);
        BEAST_EXPECT(build.suppliedExportWitnesses == 1);
        BEAST_EXPECT(build.txns.exists(fee->getTransactionID()));
        BEAST_EXPECT(build.txns.exists(amendment->getTransactionID()));
        BEAST_EXPECT(build.txns.exists(negativeUNL->getTransactionID()));
        BEAST_EXPECT(!build.txns.exists(suppliedEntropy->getTransactionID()));
        BEAST_EXPECT(!build.txns.exists(suppliedWitness->getTransactionID()));

        auto const expected =
            makeRCLTxSet(env.app(), {fee, amendment, negativeUNL});
        BEAST_EXPECT(build.txns.id() == expected.id());
        BEAST_EXPECT(agreed.exists(suppliedEntropy->getTransactionID()));
        BEAST_EXPECT(agreed.exists(suppliedWitness->getTransactionID()));

        auto const alternateAgreed = makeRCLTxSet(
            env.app(),
            {fee,
             amendment,
             negativeUNL,
             makeConsensusEntropyTx(
                 seq, makeHash("alternate-supplied-digest"), 3),
             makeExportSignaturesTx(
                 seq, makeHash("alternate-supplied-export-origin"))});
        auto const alternateBuild = ce.makeLiveBuildTxSet(alternateAgreed);
        BEAST_EXPECT(alternateAgreed.id() != agreed.id());
        BEAST_EXPECT(alternateBuild.txns.id() == build.txns.id());

        forceNonStandalone(env.app());
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        ConsensusExtensions derivation{env.app(), activeNoopJournal()};
        ConsensusExtensions alternateDerivation{env.app(), activeNoopJournal()};
        derivation.onRoundStart(RCLCxLedger{ledger}, {});
        alternateDerivation.onRoundStart(RCLCxLedger{ledger}, {});
        derivation.setRngEnabledThisRound(true);
        alternateDerivation.setRngEnabledThisRound(true);

        auto const salt = derivation.txnOrderingSalt(build.txns.id(), seq);
        auto const alternateSalt =
            alternateDerivation.txnOrderingSalt(alternateBuild.txns.id(), seq);
        BEAST_EXPECT(alternateSalt == salt);

        CanonicalTXSet derived{salt};
        CanonicalTXSet alternateDerived{alternateSalt};
        derivation.onPreBuild(derived, seq, build.txns.id());
        alternateDerivation.onPreBuild(
            alternateDerived, seq, alternateBuild.txns.id());
        auto const derivedEntropy = singleCanonicalTx(derived);
        auto const alternateEntropy = singleCanonicalTx(alternateDerived);
        BEAST_EXPECT(derivedEntropy);
        BEAST_EXPECT(alternateEntropy);
        if (derivedEntropy && alternateEntropy)
            BEAST_EXPECT(
                derivedEntropy->getFieldH256(sfDigest) ==
                alternateEntropy->getFieldH256(sfDigest));
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

        //@@start test-decorate-position-disabled-legacy
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
        //@@end test-decorate-position-disabled-legacy
    }

    void
    testExportSigGateRequiresQuorumAlignment()
    {
        testcase("Export sig gate requires quorum alignment");

        FakeExtensions ext;
        ExtensionTickHarness harness;
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
    testRngEntropyGateAllowsQuorumDespiteMissingObservation()
    {
        testcase("RNG entropy gate allows quorum despite missing observation");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingReveal;

        ExtensionTickHarness harness;
        auto const localHash = ext.entropyHash;

        harness.addEntropyPeer(1, localHash);
        harness.addEntropyPeer(2, localHash);
        harness.addEntropyPeer(3, localHash);
        harness.addEntropyPeer(4, std::nullopt);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(harness.position.entropySetHash == localHash);
        BEAST_EXPECT(ext.entropySetPublished_);

        // A silent peer did not advertise a conflicting value; once a quorum
        // has signed the same entropySetHash, waiting for that peer only gives
        // it a veto over otherwise healthy validator entropy.
        result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(!ext.entropyFailed);
        BEAST_EXPECT(harness.position.entropySetHash == localHash);
    }

    void
    testRngEntropyGateDoesNotCountUnpublishedObserverRoot()
    {
        testcase("RNG entropy gate excludes unpublished observer root");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingReveal;

        ExtensionTickHarness harness;
        harness.mode = ConsensusMode::observing;

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(harness.position.entropySetHash == ext.entropyHash);
        BEAST_EXPECT(harness.proposes == 0);

        harness.addEntropyPeer(1, ext.entropyHash);
        harness.addEntropyPeer(2, ext.entropyHash);
        harness.addEntropyPeer(3, ext.entropyHash);
        result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(!result.readyForAccept);
    }

    void
    testRngEntropyConflictAllowsQuorumDespiteMissingObservation()
    {
        testcase(
            "RNG entropy conflict allows quorum despite missing observation");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingReveal;

        ExtensionTickHarness harness;
        auto const localHash = ext.entropyHash;
        auto const conflictingHash = makeHash("conflicting-entropy-set");

        harness.addEntropyPeer(1, localHash);
        harness.addEntropyPeer(2, localHash);
        harness.addEntropyPeer(3, localHash);
        harness.addEntropyPeer(4, conflictingHash);
        harness.addEntropyPeer(5, std::nullopt);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(harness.position.entropySetHash == localHash);
        BEAST_EXPECT(ext.entropySetPublished_);

        // A quorum-aligned hash is unique over the fixed active-view
        // denominator. A below-quorum conflicting minority plus a silent peer
        // must not veto otherwise healthy validator entropy.
        result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(!ext.entropyFailed);
        BEAST_EXPECT(harness.position.entropySetHash == localHash);
    }

    void
    testRngFastPathWaitsAfterEntropyPublish()
    {
        testcase("RNG fast path waits after entropy publish");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingCommit;

        ExtensionTickHarness harness;
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
    testRngPrevProposerUnderObservationDoesNotSuppressCommitQuorum()
    {
        testcase(
            "RNG previous proposer under-observation does not suppress commit "
            "quorum");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;

        ExtensionTickHarness harness;
        harness.prevProposers = 2;

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingCommit);
        BEAST_EXPECT(ext.commitBuilds == 1);
        BEAST_EXPECT(harness.position.commitSetHash == ext.commitHash);
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

        ExtensionTickHarness harness;
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

        ExtensionTickHarness harness;
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
    testRngCommitTimeoutUsesProofedCommitsNotVisiblePeers()
    {
        testcase(
            "RNG commit timeout uses proofed commits, not visible peer "
            "count");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.commitQuorum = false;
        ext.commits = 4;

        ExtensionTickHarness harness;

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
    testRngCommitTimeoutRejectsUnproofedCommits()
    {
        testcase("RNG commit timeout rejects unproofed commits");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.commitQuorum = false;
        ext.commits = 4;
        ext.proofedCommits = 3;

        ExtensionTickHarness harness;

        auto result = harness.tick(
            ext,
            harness.parms.rngPIPELINE_TIMEOUT + std::chrono::milliseconds{1});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingTx);
        BEAST_EXPECT(ext.commitBuilds == 0);
        BEAST_EXPECT(!harness.position.commitSetHash);
    }

    void
    testRngCommitQuorumInObservingModeDoesNotPropose()
    {
        testcase("RNG commit quorum in observing mode does not propose");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;

        ExtensionTickHarness harness;
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

        ExtensionTickHarness harness;
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
    }

    void
    testRngCommitHashConflictTimeoutPublishesReveal()
    {
        testcase("RNG commit hash conflict timeout publishes reveal");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingCommit;
        ext.commitHash = makeHash("commit-local");
        ext.minimumReveals = false;

        ExtensionTickHarness harness;
        harness.start =
            std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
        harness.position.commitSetHash = ext.commitHash;
        auto const conflictHash = makeHash("commit-conflict");
        harness.addCommitPeer(1, conflictHash);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(!ext.entropyFailed);
        BEAST_EXPECT(ext.commitHashConflictStart_ == harness.start);

        result = harness.tick(
            ext,
            harness.parms.rngREVEAL_TIMEOUT + std::chrono::milliseconds{1});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(!ext.entropyFailed);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingReveal);
        BEAST_EXPECT(ext.commitFrozen);
        BEAST_EXPECT(ext.selfSeeds == 1);
        BEAST_EXPECT(harness.position.myReveal == ext.getEntropySecret());
        BEAST_EXPECT(!harness.position.entropySetHash);
        BEAST_EXPECT(ext.entropyBuilds == 0);
        BEAST_EXPECT(harness.updates == 1);
        BEAST_EXPECT(harness.proposes == 1);
        BEAST_EXPECT(
            ext.revealPhaseStart_ ==
            harness.start + harness.parms.rngREVEAL_TIMEOUT +
                std::chrono::milliseconds{1});
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

        ExtensionTickHarness harness;
        harness.position.commitSetHash = ext.commitHash;

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.estState_ == EstablishState::ConvergingReveal);
        BEAST_EXPECT(ext.commitFrozen);
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

        ExtensionTickHarness harness;

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

        ExtensionTickHarness harness;
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

        ExtensionTickHarness harness;
        harness.position.entropySetHash = ext.entropyHash;
        ext.entropySetPublished_ = true;
        ext.entropyPublishStart_ = harness.start;
        auto const conflictHash = makeHash("entropy-conflict");
        harness.addEntropyPeer(1, conflictHash);

        auto result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(!ext.entropyFailed);
        BEAST_EXPECT(harness.position.entropySetHash == ext.entropyHash);

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

        ExtensionTickHarness harness;
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
    }

    void
    testRngEntropyConflictIgnoredWithQuorumAlignment()
    {
        testcase("RNG entropy conflict ignored with quorum alignment");

        FakeExtensions ext;
        ext.rngOn = true;
        ext.exportOn = false;
        ext.estState_ = EstablishState::ConvergingReveal;

        ExtensionTickHarness harness;
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
    }

    void
    testExportSigGateAllowsAlignedQuorumDespiteMinorityConflict()
    {
        testcase("Export sig gate ignores minority conflict after quorum");

        FakeExtensions ext;
        ExtensionTickHarness harness;
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
    }

    void
    testExportSigGateAllowsQuorumDespiteMissingObservation()
    {
        testcase(
            "Export sig gate allows quorum despite missing sidecar "
            "observation");

        FakeExtensions ext;
        ExtensionTickHarness harness;
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
    testExportSigGateObservesAdvertisedPeerSets()
    {
        testcase("Export sig gate observes advertised peer sets");

        FakeExtensions ext;
        ext.localExportSigs = false;
        ext.livePendingExportLatches = true;
        ExtensionTickHarness harness;
        auto const peerHash = makeHash("peer-export-sig-set");

        harness.addPeer(1, peerHash);

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.exportSigGateStarted_);
        BEAST_EXPECT(!harness.position.exportSigSetHash);

        result = harness.tick(
            ext,
            harness.parms.rngREVEAL_TIMEOUT * 2 + std::chrono::milliseconds{1});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(ext.exportSigConvergenceFailed_);
    }

    void
    testExportSigGateIgnoresAdvertisedSetsWithoutExportTxns()
    {
        testcase("Export sig gate ignores advertised sets without export txns");

        FakeExtensions ext;
        ext.localExportSigs = false;
        ext.livePendingExportLatches = false;
        ExtensionTickHarness harness;
        auto const peerHash = makeHash("empty-round-export-sig-set");

        harness.addPeer(1, peerHash);

        auto result = harness.tick(ext);
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(!ext.exportSigGateStarted_);
        BEAST_EXPECT(!ext.exportSigConvergenceFailed_);
    }

    void
    testExportSigGateObservingModeDoesNotPropose()
    {
        testcase("Export sig gate observing mode does not propose");

        FakeExtensions ext;
        ExtensionTickHarness harness;
        harness.mode = ConsensusMode::observing;

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(harness.position.exportSigSetHash == ext.exportHash);
        BEAST_EXPECT(harness.updates == 1);
        BEAST_EXPECT(harness.proposes == 0);

        harness.addPeer(1, ext.exportHash);
        harness.addPeer(2, ext.exportHash);
        harness.addPeer(3, ext.exportHash);
        result = harness.tick(ext, std::chrono::milliseconds{100});
        BEAST_EXPECT(!result.readyForAccept);

        // An observer's unpublished local root does not count, but a complete
        // peer-only qV can still authorize materialization of its matching
        // locally held candidate.
        harness.addPeer(4, ext.exportHash);
        result = harness.tick(ext, std::chrono::milliseconds{200});
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(!ext.exportSigConvergenceFailed_);
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

        ExtensionTickHarness harness;
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
    }

    void
    testExportSigGateBoundsCandidateObservationWindow()
    {
        testcase("Export sig gate bounds candidate observation window");

        FakeExtensions ext;
        ext.localExportSigs = false;
        ext.livePendingExportLatches = true;
        ExtensionTickHarness harness;

        auto result = harness.tick(ext);
        BEAST_EXPECT(!result.readyForAccept);
        BEAST_EXPECT(ext.exportSigGateStarted_);
        BEAST_EXPECT(!harness.position.exportSigSetHash);
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
        ExtensionTickHarness harness;

        harness.addPeer(1, ext.exportHash);

        auto result = harness.tick(ext);
        BEAST_EXPECT(result.readyForAccept);
        BEAST_EXPECT(!ext.exportSigGateStarted_);
        BEAST_EXPECT(!harness.position.exportSigSetHash);
        BEAST_EXPECT(ext.exportBuilds == 0);
    }

    void
    testParticipantDiagnosticsOnlyWhenExtensionEnabled()
    {
        testcase("Participant diagnostics only when extension enabled");

        FakeExtensions ext;
        ext.rngOn = false;
        ext.exportOn = false;
        ExtensionTickHarness harness;

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
        auto const origin = makeHash("export-disabled-clears-collector");
        auto const pk = makeValidatorKeys().front();
        std::uint8_t const sigBytes[] = {1, 2, 3};
        Buffer const sig{sigBytes, sizeof(sigBytes)};

        ce.setExportEnabledThisRound(true);
        BEAST_EXPECT(
            ce.postValidationExportSigCollector().registerOrigin(origin, 10));
        auto admission =
            ce.postValidationExportSigCollector().beginAttributedAdmission(
                origin, ExportSigCollector::Contribution{0, pk, sig}, 10);
        BEAST_EXPECT(admission.ticket);
        if (admission.ticket)
            ce.postValidationExportSigCollector().admitContribution(
                std::move(*admission.ticket), true, 10);
        ce.clearRngState();
        BEAST_EXPECT(
            ce.postValidationExportSigCollector().fullUnionSnapshot().size() ==
            1);

        ce.setExportEnabledThisRound(false);
        ce.clearRngState();
        BEAST_EXPECT(
            ce.postValidationExportSigCollector().fullUnionSnapshot().empty());
    }

    void
    testValidatorKeylessAuthoringNoops()
    {
        testcase("validator-keyless extension authoring no-ops");

        using namespace jtx;
        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy | featureExport,
            nullptr};
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(!valKeys.keys);

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        auto const ledger = env.app().getLedgerMaster().getClosedLedger();

        ExtendedPosition position{makeHash("keyless-authoring")};
        ce.decoratePosition(position, ledger, true);
        BEAST_EXPECT(!position.myCommitment);
        BEAST_EXPECT(ce.pendingCommitCount() == 0);

        ce.generateEntropySecret();
        ce.selfSeedReveal();
        BEAST_EXPECT(ce.pendingRevealCount() == 0);

        protocol::TMProposeSet prop;
        RCLCxPeerPos::Proposal proposal{
            ledger->info().hash,
            0,
            position,
            NetClock::time_point{},
            NetClock::time_point{},
            beast::zero};
        ce.setExportEnabledThisRound(true);
        ce.attachExportSignatures(prop, proposal);
        BEAST_EXPECT(prop.exportsignatures_size() == 0);

        position.myCommitment = makeHash("keyless-commitment");
        position.myReveal = makeHash("keyless-reveal");
        ce.decorateMessage(prop, proposal, position, Buffer{});
        BEAST_EXPECT(ce.pendingRevealCount() == 0);
        BEAST_EXPECT(ce.pendingCommitCount() == 0);
    }

    void
    runExportStreamRetainedReplayRace(bool replayWins)
    {
        using namespace std::chrono_literals;
        using namespace jtx;

        testcase(
            replayWins ? "Export stream replay wins before an accepted edge"
                       : "Export stream accepted edge wins before replay");

        auto config = envconfig(validator, "");
        config->NETWORK_ID = 21337;
        Env env{
            *this,
            std::move(config),
            supported_amendments() | featureExport,
            nullptr};
        Account const alice{"alice"};
        Account const carol{"carol"};
        env.fund(XRP(1000), alice, carol);
        env.close();

        auto const& valKeys = env.app().getValidatorKeys();
        if (!BEAST_EXPECT(valKeys.keys))
            return;

        env.app().openLedger().modify(
            [&](OpenView& view, beast::Journal) -> bool {
                STTx tx(ttUNL_REPORT, [&](auto& obj) {
                    obj.setFieldU32(sfLedgerSequence, env.current()->seq());
                    auto active = std::make_unique<STObject>(sfActiveValidator);
                    active->setFieldVL(
                        sfPublicKey, valKeys.keys->masterPublicKey);
                    obj.set(std::move(active));
                });
                auto const txID = tx.getTransactionID();
                auto serializer = std::make_shared<Serializer>();
                tx.add(*serializer);
                env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                view.rawTxInsert(txID, std::move(serializer), nullptr);
                return true;
            });
        if (!BEAST_EXPECT(env.close(
                env.now() + std::chrono::seconds{5},
                std::chrono::milliseconds{0})))
            return;
        forceNonStandalone(env.app());

        auto& ledgerMaster = env.app().getLedgerMaster();
        auto const universe = ledgerMaster.getClosedLedger();
        if (!BEAST_EXPECT(universe && universe->read(keylet::UNLReport())))
            return;
        auto const committeeRoster =
            serializeExportCommittee({valKeys.keys->masterPublicKey});
        auto const committeeDigest =
            exportCommitteeHash(makeSlice(committeeRoster));

        auto installValidated = [&](std::shared_ptr<Ledger> const& ledger) {
            ledgerMaster.storeLedger(ledger);
            ledgerMaster.setFullLedger(ledger, false, false);
            auto const current = ledgerMaster.getValidatedLedger();
            BEAST_EXPECT(
                current && current->info().seq == ledger->info().seq &&
                current->info().hash == ledger->info().hash);
        };

        auto const originSeq = universe->info().seq + 1;
        auto innerObj = makeExportedPayment(alice.id(), carol.id());
        innerObj.setFieldU32(sfFirstLedgerSequence, originSeq);
        innerObj.setFieldU32(sfLastLedgerSequence, originSeq + 5);
        auto const innerTx = makeSTTx(innerObj);

        Json::Value exportJson;
        exportJson[jss::TransactionType] = jss::Export;
        exportJson[jss::Account] = alice.human();
        exportJson[jss::LastLedgerSequence] = originSeq;
        exportJson[sfExportedTxn.jsonName] =
            innerObj.getJson(JsonOptions::none);
        exportJson[sfExportCommitteeHash.jsonName] = to_string(committeeDigest);
        exportJson[sfExportCommittee.jsonName] = strHex(committeeRoster);
        auto const exportTx =
            env.jt(exportJson, fee(XRP(1)), ter(tesSUCCESS)).stx;
        if (!BEAST_EXPECT(exportTx))
            return;

        CanonicalTXSet originTxs{universe->info().hash};
        originTxs.insert(exportTx);
        std::set<TxID> failed;
        auto originLedger = buildLedger(
            universe,
            env.app().timeKeeper().closeTime(),
            true,
            universe->info().closeTimeResolution,
            env.app(),
            originTxs,
            failed,
            env.journal);
        BEAST_EXPECT(originTxs.empty());
        BEAST_EXPECT(failed.empty());
        auto const origin = exportTx->getTransactionID();
        auto const latchKey = keylet::exportLatch(alice.id(), origin);
        auto const pendingLatch = originLedger->read(latchKey);
        if (!BEAST_EXPECT(
                pendingLatch && pendingLatch->isFieldPresent(sfExportNode) &&
                !pendingLatch->isFieldPresent(sfExportSignatureHash) &&
                !pendingLatch->isFieldPresent(sfExportCommittee)))
            return;

        auto release = ExportOriginMemo::releaseForm(
            innerTx,
            ExportOriginMemo::Origin{env.app().config().NETWORK_ID, 0, origin},
            ExportOriginMemo::Anchor{
                originLedger->info().seq, originLedger->info().hash});
        if (!BEAST_EXPECT(release))
            return;
        auto const signature = ExportResultBuilder::signExportedTxn(
            release.value(), valKeys.keys->publicKey, valKeys.keys->secretKey);
        ExportShare const share{
            ExportShare::currentVersion,
            alice.id(),
            origin,
            originLedger->info().seq,
            originLedger->info().hash,
            0,
            valKeys.keys->publicKey,
            signature};

        auto const foreignMasterSecret =
            generateSecretKey(KeyType::secp256k1, randomSeed());
        auto const foreignSigningSecret =
            generateSecretKey(KeyType::secp256k1, randomSeed());
        auto const foreignMasterKey =
            derivePublicKey(KeyType::secp256k1, foreignMasterSecret);
        auto const foreignSigningKey =
            derivePublicKey(KeyType::secp256k1, foreignSigningSecret);
        BEAST_EXPECT(
            env.app().validatorManifests().applyManifest(makeValidatorManifest(
                foreignMasterSecret, foreignSigningSecret)) ==
            ManifestDisposition::accepted);
        auto wrongSigner = share;
        wrongSigner.signingKey = foreignSigningKey;
        wrongSigner.signature = ExportResultBuilder::signExportedTxn(
            release.value(), foreignSigningKey, foreignSigningSecret);
        auto wrongMaster = share;
        wrongMaster.signingKey = foreignMasterKey;
        wrongMaster.signature = ExportResultBuilder::signExportedTxn(
            release.value(), foreignMasterKey, foreignMasterSecret);

        auto wsc = makeWSClient(env.app().config());
        Json::Value stream;
        stream[jss::streams] = Json::arrayValue;
        stream[jss::streams].append("export_signatures");
        BEAST_EXPECT(
            wsc->invoke("subscribe", stream)[jss::status] == "success");

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.startExportShareService();

        auto hasRetainedContribution = [&] {
            auto const snapshot =
                ce.postValidationExportSigCollector().fullUnionSnapshot();
            auto const found = snapshot.find(origin);
            return found != snapshot.end() && found->second.size() == 1 &&
                found->second.front().position == share.committeePosition &&
                found->second.front().signingKey == share.signingKey &&
                found->second.front().signature == share.signature;
        };
        auto expectNoShareEvent = [&] {
            BEAST_EXPECT(!wsc->findMsg(250ms, [](Json::Value const& event) {
                return event[jss::type] == "exportSignatureReceived";
            }));
        };
        auto expectOneShareEvent = [&](Ledger const& cursor) {
            auto const event =
                wsc->findMsg(5s, [&](Json::Value const& message) {
                    return message[jss::type] == "exportSignatureReceived" &&
                        message[jss::origin_txid] == to_string(origin);
                });
            BEAST_EXPECT(event);
            if (event)
            {
                BEAST_EXPECT((*event)[jss::stream] == "export_signatures");
                BEAST_EXPECT(
                    (*event)[jss::ledger_index].asUInt() == cursor.info().seq);
                BEAST_EXPECT(
                    (*event)[jss::ledger_hash] ==
                    to_string(cursor.info().hash));
                BEAST_EXPECT(
                    (*event)[jss::committee_position].asUInt() ==
                    share.committeePosition);
            }
            expectNoShareEvent();
        };

        auto deferredCount = [&] {
            std::lock_guard lock(ce.deferredExportSharesMutex_);
            return ce.deferredExportShares_.size();
        };
        std::atomic<std::size_t> deferredCharges{0};
        std::atomic<ExportShareCharge> lastDeferredCharge{
            ExportShareCharge::none};
        auto const deferredCharge = [&](ExportShareCharge const charge) {
            lastDeferredCharge.store(charge, std::memory_order_relaxed);
            deferredCharges.fetch_add(1, std::memory_order_relaxed);
        };

        auto admission = ce.onExportShare(share, deferredCharge);
        BEAST_EXPECT(admission.disposition == ExportShareDisposition::deferred);
        BEAST_EXPECT(admission.charge == ExportShareCharge::none);
        BEAST_EXPECT(deferredCount() == 1);
        BEAST_EXPECT(!hasRetainedContribution());

        admission = ce.onExportShare(share, deferredCharge);
        BEAST_EXPECT(
            admission.disposition == ExportShareDisposition::duplicate);
        BEAST_EXPECT(deferredCount() == 1);

        auto wrongBranch = share;
        wrongBranch.originLedgerHash = makeHash("wrong-export-origin");
        admission = ce.onExportShare(wrongBranch, deferredCharge);
        BEAST_EXPECT(admission.disposition == ExportShareDisposition::deferred);
        BEAST_EXPECT(deferredCount() == 2);

        admission = ce.onExportShare(wrongSigner, deferredCharge);
        BEAST_EXPECT(admission.disposition == ExportShareDisposition::deferred);
        BEAST_EXPECT(deferredCount() == 3);

        admission = ce.onExportShare(wrongMaster, deferredCharge);
        BEAST_EXPECT(admission.disposition == ExportShareDisposition::deferred);
        BEAST_EXPECT(deferredCount() == 4);

        auto beyondHorizon = share;
        beyondHorizon.originLedgerSeq = universe->info().seq +
            ConsensusExtensions::maxDeferredExportShareFutureLedgers_ + 1;
        admission = ce.onExportShare(beyondHorizon, deferredCharge);
        BEAST_EXPECT(admission.disposition == ExportShareDisposition::deferred);
        BEAST_EXPECT(deferredCount() == 4);

        installValidated(originLedger);

        if (replayWins)
        {
            std::unique_lock streamLock{ce.exportStreamMutex_};
            std::promise<void> callbackStarted;
            auto callbackStartedFuture = callbackStarted.get_future();
            auto callback = std::async(std::launch::async, [&] {
                callbackStarted.set_value();
                ce.onValidatedLedger(
                    originLedger->info().seq, originLedger->info().hash);
            });
            callbackStartedFuture.wait();
            BEAST_EXPECT(
                callback.wait_for(50ms) == std::future_status::timeout);
            // The callback must reach retained replay before it authors this
            // node's share. Reordering local authoring ahead of replay would
            // admit the contribution while blocked on exportStreamMutex_.
            BEAST_EXPECT(!hasRetainedContribution());
            streamLock.unlock();
            auto const status = callback.wait_for(5s);
            BEAST_EXPECT(status == std::future_status::ready);
            if (status != std::future_status::ready)
                return;
            callback.get();

            // Replay snapshots first; local authoring then admits and publishes
            // the accepted edge in the same validated-ledger callback.
            BEAST_EXPECT(hasRetainedContribution());
        }
        else
        {
            std::unique_lock streamLock{ce.exportStreamMutex_};
            std::promise<void> edgeStarted;
            auto edgeStartedFuture = edgeStarted.get_future();
            auto edge = std::async(std::launch::async, [&] {
                edgeStarted.set_value();
                return ce.onExportShare(share);
            });
            edgeStartedFuture.wait();
            auto const deadline = std::chrono::steady_clock::now() + 5s;
            while (!hasRetainedContribution() &&
                   edge.wait_for(0ms) == std::future_status::timeout &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            BEAST_EXPECT(hasRetainedContribution());
            BEAST_EXPECT(edge.wait_for(0ms) == std::future_status::timeout);
            streamLock.unlock();
            auto const status = edge.wait_for(5s);
            BEAST_EXPECT(status == std::future_status::ready);
            if (status != std::future_status::ready)
                return;
            BEAST_EXPECT(edge.get());

            ce.onValidatedLedger(
                originLedger->info().seq, originLedger->info().hash);
        }

        BEAST_EXPECT(hasRetainedContribution());
        admission = ce.onExportShare(share, {});
        BEAST_EXPECT(
            admission.disposition == ExportShareDisposition::duplicate);
        BEAST_EXPECT(admission.charge == ExportShareCharge::none);

        admission = ce.onExportShare(wrongBranch, {});
        BEAST_EXPECT(admission.disposition == ExportShareDisposition::invalid);
        BEAST_EXPECT(admission.charge == ExportShareCharge::invalidData);

        admission = ce.onExportShare(wrongSigner, {});
        BEAST_EXPECT(admission.disposition == ExportShareDisposition::invalid);
        BEAST_EXPECT(admission.charge == ExportShareCharge::invalidData);

        admission = ce.onExportShare(wrongMaster, {});
        BEAST_EXPECT(admission.disposition == ExportShareDisposition::invalid);
        BEAST_EXPECT(admission.charge == ExportShareCharge::invalidData);

        auto invalidSignature = share;
        invalidSignature.signature = sign(
            valKeys.keys->publicKey,
            valKeys.keys->secretKey,
            Slice{"invalid-export-share", 20});
        admission = ce.onExportShare(invalidSignature, {});
        BEAST_EXPECT(admission.disposition == ExportShareDisposition::invalid);
        BEAST_EXPECT(admission.charge == ExportShareCharge::invalidSignature);

        BEAST_EXPECT(deferredCount() == 0);
        BEAST_EXPECT(deferredCharges.load(std::memory_order_relaxed) == 3);
        BEAST_EXPECT(
            lastDeferredCharge.load(std::memory_order_relaxed) ==
            ExportShareCharge::invalidData);
        BEAST_EXPECT(
            ce.lastExportReplaySeq_.load(std::memory_order_relaxed) ==
            originLedger->info().seq);
        expectOneShareEvent(*originLedger);

        // Model admission retaining the preceding validated cursor while the
        // validated callback has already completed this origin's retry pass.
        // The share must be admitted directly rather than queued behind that
        // completed pass.
        ce.postValidationExportSigCollector().clear(origin);
        {
            std::lock_guard lock(ce.deferredExportSharesMutex_);
            ce.deferredExportShareRetrySeq_ = originLedger->info().seq;
        }
        admission = ce.deferExportShare(share, {}, universe->info().seq);
        BEAST_EXPECT(admission.disposition == ExportShareDisposition::accepted);
        BEAST_EXPECT(deferredCount() == 0);
        BEAST_EXPECT(hasRetainedContribution());

        auto next = std::make_shared<Ledger>(
            *originLedger, env.app().timeKeeper().closeTime());
        next->updateSkipList();
        next->setAccepted(
            next->info().closeTime, next->info().closeTimeResolution, true);
        installValidated(next);
        ce.onValidatedLedger(next->info().seq, next->info().hash);
        expectOneShareEvent(*next);
        ce.onValidatedLedger(next->info().seq, next->info().hash);
        expectNoShareEvent();

        auto witnessed =
            std::make_shared<Ledger>(*next, env.app().timeKeeper().closeTime());
        Sandbox witnessChanges{witnessed.get(), tapNONE};
        BEAST_EXPECT(isTesSuccess(ExportLedgerOps::recordExportWitness(
            witnessChanges,
            witnessChanges,
            latchKey,
            makeHash("stream-witness"),
            env.journal)));
        witnessChanges.apply(*witnessed);
        witnessed->updateSkipList();
        witnessed->setAccepted(
            witnessed->info().closeTime,
            witnessed->info().closeTimeResolution,
            true);
        installValidated(witnessed);

        auto const terminalLatch = witnessed->read(latchKey);
        BEAST_EXPECT(
            terminalLatch && !terminalLatch->isFieldPresent(sfExportNode) &&
            terminalLatch->isFieldPresent(sfExportSignatureHash));
        BEAST_EXPECT(hasRetainedContribution());
        ce.onValidatedLedger(witnessed->info().seq, witnessed->info().hash);
        BEAST_EXPECT(!ce.onExportShare(share));
        BEAST_EXPECT(hasRetainedContribution());
        expectNoShareEvent();

        auto pendingAtStop = share;
        pendingAtStop.originLedgerSeq = witnessed->info().seq + 1;
        pendingAtStop.originLedgerHash = makeHash("pending-at-stop");
        admission = ce.onExportShare(pendingAtStop, deferredCharge);
        BEAST_EXPECT(admission.disposition == ExportShareDisposition::deferred);
        BEAST_EXPECT(deferredCount() == 1);

        ce.stopExportShareService();
        BEAST_EXPECT(deferredCount() == 0);
        BEAST_EXPECT(
            wsc->invoke("unsubscribe", stream)[jss::status] == "success");
    }

    void
    testExportStreamRetainedReplayRace()
    {
        runExportStreamRetainedReplayRace(false);
        runExportStreamRetainedReplayRace(true);
    }

    void
    testExportStreamStopFence()
    {
        using namespace std::chrono_literals;
        using namespace jtx;

        testcase("Export stream stop fences a waiting validated callback");

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureExport,
            nullptr};
        auto wsc = makeWSClient(env.app().config());
        Json::Value stream;
        stream[jss::streams] = Json::arrayValue;
        stream[jss::streams].append("export_signatures");
        BEAST_EXPECT(
            wsc->invoke("subscribe", stream)[jss::status] == "success");

        ConsensusExtensions ce{env.app(), activeNoopJournal()};
        ce.startExportShareService();
        auto const validated = env.app().getLedgerMaster().getValidatedLedger();
        if (!BEAST_EXPECT(validated))
            return;

        auto const origin = makeHash("stop-fenced-pending-export");
        ce.exportStreamEmissionSeq_ = validated->info().seq;
        ce.exportStreamEmittedShares_.emplace(origin, 1);

        std::unique_lock streamLock{ce.exportStreamMutex_};
        std::promise<void> callbackStarted;
        auto callbackStartedFuture = callbackStarted.get_future();
        auto callback = std::async(std::launch::async, [&] {
            callbackStarted.set_value();
            ce.onValidatedLedger(validated->info().seq, validated->info().hash);
        });
        callbackStartedFuture.wait();
        BEAST_EXPECT(callback.wait_for(50ms) == std::future_status::timeout);

        auto stop = std::async(
            std::launch::async, [&] { ce.stopExportShareService(); });
        auto const deadline = std::chrono::steady_clock::now() + 1s;
        while (ce.exportShareServiceStarted_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        BEAST_EXPECT(
            !ce.exportShareServiceStarted_.load(std::memory_order_acquire));
        BEAST_EXPECT(stop.wait_for(0ms) == std::future_status::timeout);

        streamLock.unlock();
        BEAST_EXPECT(callback.wait_for(5s) == std::future_status::ready);
        callback.get();
        BEAST_EXPECT(stop.wait_for(5s) == std::future_status::ready);
        stop.get();

        BEAST_EXPECT(ce.exportStreamEmissionSeq_ == 0);
        BEAST_EXPECT(ce.exportStreamEmittedShares_.empty());
        BEAST_EXPECT(!wsc->findMsg(250ms, [](Json::Value const& event) {
            return event[jss::type] == "exportSignatureReceived";
        }));
        BEAST_EXPECT(
            wsc->invoke("unsubscribe", stream)[jss::status] == "success");
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
        BEAST_EXPECT(ce.exportRootAlignmentThreshold() == 1);

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
        //@@start test-export-attachment-disabled
        disabled.attachExportSignatures(prop, proposal);
        BEAST_EXPECT(prop.exportsignatures_size() == 0);

        ConsensusTestConfig cfg;
        cfg.noExportSig = true;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);
        ce.attachExportSignatures(prop, proposal);
        BEAST_EXPECT(prop.exportsignatures_size() == 0);
        //@@end test-export-attachment-disabled
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
        BEAST_EXPECT(
            env.app().getInboundTransactions().getSet(commitHash, false));

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
        BEAST_EXPECT(
            env.app().getInboundTransactions().getSet(entropyHash, false));
    }

public:
    void
    run() override
    {
        testSidecarPeerAlignmentHelper();
        testSidecarSplitBrainEquivocationThreshold();
        testActiveValidatorViewBuilderPrefersUNLReport();
        testActiveValidatorViewBuilderFallback();
        testActiveValidatorViewAppliesNegativeUNL();
        testActiveValidatorViewCapsNegativeUNL();
        testActiveValidatorViewNullSourceAndExpectedProposers();
        testParticipantThreshold();
        testThresholdPolicyHelpers();
        testRuntimeConfigPolicyAccessors();
        testOnRoundStartRefreshesFeatureLatches();
        testDecoratePositionGeneratesCommitment();
        testOnPreBuildInjectsZeroEntropyFallback();
        testTxnOrderingSaltExtendsLegacySalt();
        testOnPreBuildInjectsEntropySetEntropy();
        testOnPreBuildCanonicalizesMultiContributorEntropy();
        testOnPreBuildAcceptedEntropySetOverridesLocalFailureFlag();
        testOnPreBuildRejectsForgedEntropyContributorAttribution();
        testOnPreBuildTier2ParticipantAligned();
        testTier2ThresholdAnchorsToOriginalView();
        testOnPreBuildTier2WithNegativeUNL();
        testProposalProofRoundTrip();
        testProposalPrecheckUsesExportShareRelayLimits();
        testHarvestRngDataReplacementAndRejection();
        testRngManifestArrivalDoesNotRetargetProofedCommit();
        testRngManifestRotationDropsPinnedContributor();
        testExportCollectorBuildsAttributedUnion();
        testExportSidecarCandidateDeadline();
        testTransactionAcquireRejectsSidecarWireNodes();
        testAcquiredSetsRejectConsensusExtensionPseudos();
        testAgreedExportWitnessBuildsContributorBitmap();
        testRngSidecarBuildsLocalSnapshots();
        testOnPreBuildInjectsStandaloneEntropy();
        testOnPreBuildStripsSuppliedExtensionPseudos();
        testLiveBuildSetStripsOnlyExtensionPseudos();
        testDiagnosticsJsonAndPositionLogging();
        testDecoratePositionSkipsWhenDisabled();
        testExportSigGateRequiresQuorumAlignment();
        testRngEntropyGateAllowsQuorumDespiteMissingObservation();
        testRngEntropyGateDoesNotCountUnpublishedObserverRoot();
        testRngEntropyConflictAllowsQuorumDespiteMissingObservation();
        testRngFastPathWaitsAfterEntropyPublish();
        testRngPrevProposerUnderObservationDoesNotSuppressCommitQuorum();
        testRngCommitWaitsWhenQuorumPossible();
        testRngCommitTimeoutWithEntropyGatePublishesCommitSet();
        testRngCommitTimeoutUsesProofedCommitsNotVisiblePeers();
        testRngCommitTimeoutRejectsUnproofedCommits();
        testRngCommitQuorumInObservingModeDoesNotPropose();
        testRngCommitConflictRefreshesHashBeforeWaiting();
        testRngCommitHashConflictTimeoutPublishesReveal();
        testRngRevealTransitionWaitsWhenRevealsIncomplete();
        testRngRevealTimeoutWithoutRevealsFallsBack();
        testRngEntropyPublishIsIdempotent();
        testRngEntropyConflictTimeoutClearsHash();
        testRngEntropyConflictRefreshesHashBeforeWaiting();
        testRngEntropyConflictIgnoredWithQuorumAlignment();
        testExportSigGateAllowsAlignedQuorumDespiteMinorityConflict();
        testExportSigGateAllowsQuorumDespiteMissingObservation();
        testExportSigGateObservesAdvertisedPeerSets();
        testExportSigGateIgnoresAdvertisedSetsWithoutExportTxns();
        testExportSigGateObservingModeDoesNotPropose();
        testExportSigGateRefreshesHashBeforeWaiting();
        testExportSigGateBoundsCandidateObservationWindow();
        testExportSigGateSkipsWhenExportDisabled();
        testParticipantDiagnosticsOnlyWhenExtensionEnabled();
        testExportDisabledRoundClearsCollector();
        testValidatorKeylessAuthoringNoops();
        testExportStreamRetainedReplayRace();
        testExportStreamStopFence();
        testPublicHookNoopAndFailureBranches();
        testDecorateMessageStoresSelfProofs();
    }
};

BEAST_DEFINE_TESTSUITE(ConsensusExtensions, consensus, ripple);

}  // namespace test
}  // namespace ripple
