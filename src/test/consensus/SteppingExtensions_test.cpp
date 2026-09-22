#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>
#include <test/jtx/pay.h>
#include <test/unit_test/SuiteJournal.h>

#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/consensus/ConsensusExtensionsTick.h>
#include <xrpld/consensus/ConsensusParms.h>
#include <xrpld/overlay/Message.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/basics/scope.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/ExportCommittee.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/ExportShare.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/SidecarType.h>
#include <xrpl/protocol/TxFlags.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace ripple::test {

class SteppingExtensions_test : public beast::unit_test::suite
{
    static constexpr std::uint32_t networkID = 21337;
    static constexpr std::uint32_t observer = 3;
    // The first flag vote has fewer than 256 ancestors and only establishes
    // validation retention. The second can score a complete history window.
    static constexpr std::uint32_t warmLedger = 2 * FLAG_LEDGER_INTERVAL + 1;

    enum class Fault {
        none,
        noDirectShares,
        noProposalShares,
        noShares,
        duplicateTraffic,
        slowObserver,
        lateValidations,
        veryLateValidations,
        slowValidator,
        slowValidatorIdleExport,
        delayedRngReveals,
        delayedExportCallbacks,
        reorderShares
    };

    struct Observations
    {
        // Seq, parent hash, built hash, captured before acquisition can replace
        // the node's by-sequence ledger lookup. A set deduplicates peer sends.
        using Build = std::tuple<std::uint32_t, uint256, uint256>;
        std::array<std::set<Build>, 4> builds;
        std::array<std::set<uint256>, 4> acquiredHashes;
        std::array<std::size_t, 4> secrets{};
        std::array<std::size_t, 4> directFrames{};
        std::array<std::size_t, 4> proposalFrames{};
        std::array<std::size_t, 4> exportRootFrames{};
        std::array<std::size_t, 4> ownReleases{};
        std::array<std::size_t, 4> unauthorizedReleases{};
        std::uint32_t droppedDirect = 0;
        std::uint32_t droppedProposals = 0;
        std::uint32_t duplicatedFrames = 0;
        std::uint32_t delayedValidations = 0;
        std::uint32_t delayedValidatorTraffic = 0;
        std::uint32_t validationGap = 0;
        std::uint32_t validatedAheadOfClosed = 0;
        std::uint32_t delayedReveals = 0;
        std::uint32_t exportAheadOfRng = 0;
        std::uint32_t rngAheadOfExport = 0;
        std::uint32_t gatedAccepts = 0;
        std::uint32_t serviceLag = 0;
        std::uint32_t reorderedShares = 0;
        std::uint32_t invertedSharePairs = 0;
        std::vector<uint256> shareSendOrder;
        std::vector<uint256> shareRecvOrder;
        std::uint32_t droppedStarvedDirect = 0;
        std::uint32_t droppedStarvedProposals = 0;
        bool sawPartialCandidate = false;
        bool sawObserverMap = false;
        std::size_t candidateLeavesA = 0;
        std::size_t candidateLeavesB = 0;
        bool queuedExportWork = false;
        bool oldGenerationRan = false;
        bool newGenerationRan = false;
        std::optional<uint256> captureOrigin;
        std::optional<ExportShare> honestShare;
        std::string badShareBytes;
        Buffer badSignature;
        std::uint32_t badFramesReceived = 0;
        bool sawBadWhilePending = false;
        std::map<uint256, std::array<std::size_t, 4>> originOwnReleases;
    };

    template <class T>
    static std::shared_ptr<T>
    decodeFrame(SimPipe::Frame frame)
    {
        if (frame.empty())
            throw std::logic_error("Empty fixture frame");
        auto const buffer = boost::asio::buffer(frame.data(), frame.size());
        boost::system::error_code ec;
        auto const header =
            ripple::detail::parseMessageHeader(ec, buffer, frame.size());
        if (ec || !header || header->total_wire_size != frame.size())
            throw std::logic_error("Invalid fixture frame header");
        auto result = ripple::detail::parseMessageContent<T>(*header, buffer);
        if (!result)
            throw std::logic_error("Invalid fixture frame payload");
        return result;
    }

    static uint256
    directBatchId(protocol::TMExportShares const& batch)
    {
        Serializer s;
        for (auto const& share : batch.shares())
            s.addRaw(share.data(), share.size());
        return s.getSHA512Half();
    }

    static std::optional<std::size_t>
    originSidecarLeaves(SHAMap const& map, uint256 const& origin)
    {
        std::set<std::uint32_t> positions;
        bool malformed = false;
        map.visitLeaves(
            [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                if (malformed)
                    return;
                try
                {
                    SerialIter sit{item->slice()};
                    STObject const obj{sit, sfGeneric};
                    if (!obj.isFieldPresent(sfSidecarType) ||
                        obj.getFieldU8(sfSidecarType) != sidecarExportSig ||
                        !obj.isFieldPresent(sfTransactionHash) ||
                        !obj.isFieldPresent(sfTransactionIndex) ||
                        !obj.isFieldPresent(sfSigningPubKey) ||
                        !obj.isFieldPresent(sfTxnSignature))
                    {
                        malformed = true;
                        return;
                    }
                    if (obj.getFieldH256(sfTransactionHash) != origin)
                        return;
                    if (!positions.insert(obj.getFieldU32(sfTransactionIndex))
                             .second)
                        malformed = true;
                }
                catch (...)
                {
                    malformed = true;
                }
            });
        if (malformed)
            return std::nullopt;
        return positions.size();
    }

    static bool
    authorizedAtEmission(Application& app, ExportShare const& share)
    {
        auto const validated = app.getLedgerMaster().getValidatedLedger();
        if (!validated || validated->seq() < share.originLedgerSeq)
            return false;
        auto const hash = validated->seq() == share.originLedgerSeq
            ? std::optional<uint256>{validated->info().hash}
            : hashOfSeq(
                  *validated, share.originLedgerSeq, app.journal("Ledger"));
        return hash && *hash == share.originLedgerHash;
    }

    static bool
    ledgerHasTx(std::shared_ptr<Ledger const> const& ledger, uint256 const& id)
    {
        if (!ledger)
            return false;
        for (auto const& [tx, meta] : ledger->txs)
            if (tx->getTransactionID() == id)
                return true;
        return false;
    }

    static bool
    pendingDirContains(Ledger const& ledger, uint256 const& key)
    {
        bool found = false;
        forEachItem(
            ledger,
            keylet::pendingExports(),
            [&](std::shared_ptr<SLE const> const& sle) {
                if (sle && sle->key() == key)
                    found = true;
            });
        return found;
    }

    static std::optional<TER>
    validatedResult(SteppingNetwork& net, uint256 const& id)
    {
        for (auto seq = warmLedger; seq <= net.minValidatedSeq(); ++seq)
        {
            auto const ledger = net.ledger(0, seq);
            if (!ledger)
                continue;
            for (auto const& [tx, meta] : ledger->txs)
            {
                if (tx->getTransactionID() != id || !meta ||
                    !meta->isFieldPresent(sfTransactionResult))
                    continue;
                return TER::fromInt(meta->getFieldU8(sfTransactionResult));
            }
        }
        return std::nullopt;
    }

    struct World
    {
        SteppingNetwork& net;
        bool rng;
        bool exportEnabled;
        std::shared_ptr<Observations> observed =
            std::make_shared<Observations>();
        jtx::Account owner{"dsf-export-owner"};
        jtx::Account destination{"dsf-export-destination"};

        World(SteppingNetwork& network, bool rngOn, bool exportOn)
            : net(network), rng(rngOn), exportEnabled(exportOn)
        {
            net.configureNodes([stats = observed, rngOn, exportOn](
                                   std::uint32_t id, Config& cfg) {
                cfg.NETWORK_ID = networkID;
                // Let the real flag-ledger vote produce the common active view.
                cfg.features.insert(featureNegativeUNL);
                cfg.features.insert(featureXahauGenesis);
                if (rngOn)
                    cfg.features.insert(featureConsensusEntropy);
                if (exportOn)
                    cfg.features.insert(featureExport);
                cfg.harnessEntropySecret = [stats, id](
                                               uint256 const& parent,
                                               std::uint32_t seq) {
                    ++stats->secrets.at(id);
                    return sha512Half(
                        std::string{"DSF entropy fixture"}, id, parent, seq);
                };
                cfg.harnessPeerMessage = [stats, id](
                                             std::uint16_t type,
                                             std::string const&,
                                             std::uint32_t,
                                             beast::IP::Endpoint const&,
                                             ::google::protobuf::Message const&
                                                 msg) {
                    if (type == protocol::mtEXPORT_SHARES)
                    {
                        ++stats->directFrames.at(id);
                        auto const& batch =
                            static_cast<protocol::TMExportShares const&>(msg);
                        if (id == observer)
                            stats->shareRecvOrder.push_back(
                                directBatchId(batch));
                        for (auto const& blob : batch.shares())
                        {
                            if (id == observer && stats->captureOrigin &&
                                !stats->honestShare)
                            {
                                auto const share =
                                    ExportShare::parse(makeSlice(blob));
                                if (share &&
                                    share->originTxn == *stats->captureOrigin)
                                    stats->honestShare = share;
                            }
                            if (id == observer &&
                                !stats->badShareBytes.empty() &&
                                blob == stats->badShareBytes)
                                ++stats->badFramesReceived;
                        }
                    }
                    if (type == protocol::mtLEDGER_DATA)
                    {
                        auto const& data =
                            static_cast<protocol::TMLedgerData const&>(msg);
                        if (data.ledgerhash().size() == uint256::bytes)
                        {
                            uint256 hash;
                            std::copy(
                                data.ledgerhash().begin(),
                                data.ledgerhash().end(),
                                hash.begin());
                            stats->acquiredHashes.at(id).insert(hash);
                        }
                    }
                    if (type == protocol::mtPROPOSE_LEDGER)
                    {
                        auto const& proposal =
                            static_cast<protocol::TMProposeSet const&>(msg);
                        if (proposal.exportsignatures_size())
                            ++stats->proposalFrames.at(id);
                        SerialIter iter{makeSlice(proposal.currenttxhash())};
                        auto const position = ExtendedPosition::fromSerialIter(
                            iter, proposal.currenttxhash().size());
                        if (position && position->exportSigSetHash)
                            ++stats->exportRootFrames.at(id);
                    }
                };
            });
            net.validators(3);
            net.observer();
            net.mesh();
            for (std::uint32_t id = 0; id <= observer; ++id)
            {
                auto const prior = net.node(id).app().config().harnessPeerSend;
                net.raw().setPeerSendHook(
                    id,
                    [stats = observed, id, prior, nodes = &net.raw()](
                        std::uint16_t type,
                        std::string const& name,
                        std::uint32_t peer,
                        beast::IP::Endpoint const& remote,
                        std::string const& stage,
                        Message& message) {
                        if (prior)
                            prior(type, name, peer, remote, stage, message);
                        if (stage == "call" && nodes->isLive(id) &&
                            (type == protocol::mtEXPORT_SHARES ||
                             type == protocol::mtPROPOSE_LEDGER))
                        {
                            auto& app = (*nodes)[id].app();
                            auto const& keys = app.getValidatorKeys();
                            auto const observe = [&](std::string const& bytes) {
                                auto const share =
                                    ExportShare::parse(makeSlice(bytes));
                                if (share && keys.keys &&
                                    share->signingKey == keys.keys->publicKey)
                                {
                                    ++stats->ownReleases[id];
                                    ++stats->originOwnReleases[share->originTxn]
                                                              [id];
                                    if (!authorizedAtEmission(app, *share))
                                        ++stats->unauthorizedReleases[id];
                                }
                            };
                            auto const& bytes =
                                message.getBuffer(compression::Compressed::Off);
                            if (type == protocol::mtEXPORT_SHARES)
                            {
                                auto const batch =
                                    decodeFrame<protocol::TMExportShares>(
                                        bytes);
                                for (auto const& share : batch->shares())
                                    observe(share);
                            }
                            else
                            {
                                auto const proposal =
                                    decodeFrame<protocol::TMProposeSet>(bytes);
                                for (auto const& share :
                                     proposal->exportsignatures())
                                    observe(share);
                            }
                        }
                        if (type != protocol::mtSTATUS_CHANGE ||
                            stage != "call")
                            return;
                        // Uncompressed protocol frames have a six-byte header.
                        auto const& frame =
                            message.getBuffer(compression::Compressed::Off);
                        protocol::TMStatusChange status;
                        if (frame.size() < 6 ||
                            !status.ParseFromArray(
                                frame.data() + 6,
                                static_cast<int>(frame.size() - 6)))
                            throw std::logic_error(
                                "Invalid fixture status frame");
                        if (status.newevent() != protocol::neACCEPTED_LEDGER ||
                            status.ledgerseq() <= warmLedger + 2)
                            return;
                        if (status.ledgerhashprevious().size() !=
                                uint256::bytes ||
                            status.ledgerhash().size() != uint256::bytes)
                            throw std::logic_error(
                                "Invalid fixture status hashes");
                        uint256 parent, hash;
                        std::copy(
                            status.ledgerhashprevious().begin(),
                            status.ledgerhashprevious().end(),
                            parent.begin());
                        std::copy(
                            status.ledgerhash().begin(),
                            status.ledgerhash().end(),
                            hash.begin());
                        stats->builds.at(id).emplace(
                            status.ledgerseq(), parent, hash);
                    });
            }
        }

        std::shared_ptr<Transaction>
        submit(std::uint32_t node, Json::Value tx, jtx::Account const& signer)
        {
            tx[jss::NetworkID] = networkID;
            return net.submit(node, std::move(tx), signer);
        }

        Json::Value
        intent(
            jtx::Account const& acct,
            std::uint32_t ticket,
            std::uint32_t lastLedger,
            std::vector<std::uint32_t> const& slots = {0, 1, 2})
        {
            std::vector<PublicKey> keys;
            for (auto const i : slots)
                keys.push_back(
                    net.node(i).app().getValidatorKeys().keys->masterPublicKey);
            auto const roster = canonicalizeExportCommittee(
                makeSlice(serializeExportCommittee(keys)));
            if (!roster)
                throw std::logic_error("Invalid fixture committee");

            STObject inner(sfExportedTxn);
            inner.setFieldU16(sfTransactionType, ttPAYMENT);
            inner.setFieldU32(sfFlags, tfFullyCanonicalSig);
            inner.setFieldU32(sfSequence, 0);
            inner.setFieldU32(sfTicketSequence, ticket);
            inner.setFieldU32(sfLastLedgerSequence, 1'000'000);
            inner.setFieldAmount(sfAmount, XRPAmount{1'000'000});
            inner.setFieldAmount(sfFee, XRPAmount{1000});
            inner.setFieldVL(sfSigningPubKey, Blob{});
            inner.setAccountID(sfAccount, acct.id());
            inner.setAccountID(sfDestination, destination.id());

            Json::Value tx;
            tx[jss::TransactionType] = jss::Export;
            tx[jss::Account] = acct.human();
            tx[jss::Fee] = "1000000";
            tx[jss::LastLedgerSequence] = lastLedger;
            tx[sfExportedTxn.jsonName] = inner.getJson(JsonOptions::none);
            tx[sfExportCommittee.jsonName] = strHex(*roster);
            tx[sfExportCommitteeHash.jsonName] =
                to_string(exportCommitteeHash(makeSlice(*roster)));
            return tx;
        }

        Json::Value
        intent(std::uint32_t ticket = 1)
        {
            return intent(
                owner,
                ticket,
                net.node(observer).app().openLedger().current()->seq() +
                    ExportLimits::maxAdmissionWindowLedgers);
        }
    };

    bool
    ready(World& world)
    {
        auto& net = world.net;
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return false;
        net.runTo(
            warmLedger,
            SteppingNetwork::RunBudget{(warmLedger + 32) * 60, 2'000'000});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger))
        {
            log << net.jobDiagnostics() << std::endl;
            return false;
        }
        for (std::uint32_t i = 0; i <= observer; ++i)
        {
            auto const ledger = net.ledger(i, warmLedger);
            if (!BEAST_EXPECT(ledger != nullptr))
                return false;
            BEAST_EXPECT(
                ledger->rules().enabled(featureConsensusEntropy) == world.rng);
            BEAST_EXPECT(
                ledger->rules().enabled(featureExport) == world.exportEnabled);
            auto const report = ledger->read(keylet::UNLReport());
            if (!BEAST_EXPECT(
                    report &&
                    report->getFieldArray(sfActiveValidators).size() ==
                        observer))
            {
                log << "active-view warmup: node=" << i << " seq=" << warmLedger
                    << " report=" << static_cast<bool>(report) << " members="
                    << (report
                            ? report->getFieldArray(sfActiveValidators).size()
                            : 0)
                    << std::endl;
                return false;
            }
        }
        BEAST_EXPECT(net.ledgersAgree(warmLedger));
        BEAST_EXPECT(!net.node(observer).app().getValidatorKeys().keys);
        BEAST_EXPECT(world.observed->secrets[observer] == 0);
        return true;
    }

    std::uint32_t
    witnessAt(
        SteppingNetwork& net,
        uint256 const& origin,
        std::uint32_t from,
        std::uint32_t node = observer)
    {
        auto const last = net.validSeq(node);
        for (auto seq = from; seq <= last; ++seq)
            if (auto const ledger = net.ledger(node, seq))
                for (auto const& [tx, meta] : ledger->txs)
                    if (tx->getTxnType() == ttEXPORT_SIGNATURES &&
                        tx->getFieldH256(sfTransactionHash) == origin)
                        return seq;
        return 0;
    }

    std::optional<std::vector<uint256>>
    scenario(
        SteppingNetwork& net,
        bool rng,
        bool exportOn,
        Fault fault = Fault::none)
    {
        World world(net, rng, exportOn);
        if (!ready(world))
            return std::nullopt;

        auto const funding = world.submit(
            observer,
            jtx::pay(jtx::Account::master, world.owner, jtx::XRP(10'000)),
            jtx::Account::master);
        if (!BEAST_EXPECT(funding->getResult() == tesSUCCESS))
            return std::nullopt;
        net.runTo(warmLedger + 2);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger + 2))
            return std::nullopt;

        using namespace std::chrono_literals;
        net.recordChainHistory();
        auto const stats = world.observed;
        bool const sluggish = fault == Fault::slowValidator ||
            fault == Fault::slowValidatorIdleExport;
        bool const late = fault == Fault::lateValidations ||
            fault == Fault::veryLateValidations;
        bool const createIntent =
            exportOn && fault != Fault::slowValidatorIdleExport;
        bool const gateTest = fault == Fault::delayedRngReveals ||
            fault == Fault::delayedExportCallbacks;
        if (gateTest)
        {
            // Read the real accepted evidence; never seed roots or replace a
            // callback. Job-boundary inspection also sees the state at build.
            net.controller().observeJobs(
                [this, &net, stats](
                    std::uint32_t id, JobType type, std::string const&) {
                    if (!net.isLive(id))
                        return;
                    auto& ce = net.node(id).app().getConsensusExtensions();
                    auto const v = net.validSeq(id);
                    auto const serviced =
                        ce.lastExportReplaySeq_.load(std::memory_order_relaxed);
                    if (v > serviced)
                        stats->serviceLag =
                            std::max(stats->serviceLag, v - serviced);
                    if (!ce.hasEligiblePendingExports())
                        return;
                    if (ce.acceptedExportSigSetHash_ &&
                        !ce.acceptedEntropySetHash_ &&
                        ce.estState_ == EstablishState::ConvergingReveal &&
                        !ce.hasMinimumReveals())
                        ++stats->exportAheadOfRng;
                    if (ce.acceptedEntropySetHash_ &&
                        !ce.acceptedExportSigSetHash_ &&
                        ce.exportSigGateStarted_ &&
                        !ce.exportSigConvergenceFailed())
                        ++stats->rngAheadOfExport;
                    if (type == jtACCEPT)
                    {
                        ++stats->gatedAccepts;
                        BEAST_EXPECT(
                            ce.acceptedEntropySetHash_ || ce.entropyFailed_);
                        BEAST_EXPECT(
                            ce.acceptedExportSigSetHash_ ||
                            ce.exportSigConvergenceFailed());
                    }
                });
        }
        if (fault == Fault::delayedExportCallbacks)
            for (std::uint32_t id = 0; id <= observer; ++id)
                net.controller().setJobLag(
                    id, jtADVANCE, "validatedLedgerWork", 20s);
        if (fault == Fault::delayedRngReveals)
            for (std::uint32_t from = 1; from < observer; ++from)
                net.faultFrames(
                    from, 0, [stats](std::uint16_t type, SimPipe::Frame bytes) {
                        SimFault f;
                        if (type == protocol::mtPROPOSE_LEDGER)
                        {
                            auto const proposal =
                                decodeFrame<protocol::TMProposeSet>(bytes);
                            SerialIter iter{
                                makeSlice(proposal->currenttxhash())};
                            auto const pos = ExtendedPosition::fromSerialIter(
                                iter, proposal->currenttxhash().size());
                            if (pos && pos->myReveal)
                            {
                                ++stats->delayedReveals;
                                f.delay = 1s;
                            }
                        }
                        return f;
                    });
        if (fault == Fault::noDirectShares || late ||
            fault == Fault::duplicateTraffic)
            for (std::uint32_t from = 0; from < observer; ++from)
                net.faultLink(
                    from,
                    observer,
                    [stats, fault](std::uint16_t type, std::size_t) {
                        SimFault f;
                        if (fault == Fault::noDirectShares &&
                            type == protocol::mtEXPORT_SHARES)
                        {
                            ++stats->droppedDirect;
                            f.drop = true;
                        }
                        if (fault == Fault::lateValidations &&
                            type == protocol::mtVALIDATION)
                        {
                            ++stats->delayedValidations;
                            f.delay = 5s;
                        }
                        if (fault == Fault::veryLateValidations &&
                            type == protocol::mtVALIDATION)
                        {
                            ++stats->delayedValidations;
                            f.delay = 20s;
                        }
                        if (fault == Fault::duplicateTraffic &&
                            (type == protocol::mtEXPORT_SHARES ||
                             type == protocol::mtPROPOSE_LEDGER ||
                             type == protocol::mtVALIDATION))
                        {
                            ++stats->duplicatedFrames;
                            f.duplicates = 2;
                        }
                        return f;
                    });
        if (fault == Fault::noProposalShares || fault == Fault::noShares)
            for (std::uint32_t from = 0; from < observer; ++from)
                net.faultFrames(
                    from,
                    observer,
                    [stats, fault](std::uint16_t type, SimPipe::Frame bytes) {
                        SimFault f;
                        if (type == protocol::mtPROPOSE_LEDGER &&
                            decodeFrame<protocol::TMProposeSet>(bytes)
                                    ->exportsignatures_size() != 0)
                        {
                            ++stats->droppedProposals;
                            f.drop = true;
                        }
                        if (fault == Fault::noShares &&
                            type == protocol::mtEXPORT_SHARES)
                        {
                            ++stats->droppedDirect;
                            f.drop = true;
                        }
                        return f;
                    });
        if (fault == Fault::reorderShares)
        {
            using namespace std::chrono_literals;
            for (std::uint32_t from = 0; from < observer; ++from)
                net.faultFrames(
                    from,
                    observer,
                    [stats, from](std::uint16_t type, SimPipe::Frame bytes) {
                        SimFault f;
                        if (type != protocol::mtEXPORT_SHARES)
                            return f;
                        auto const batch =
                            decodeFrame<protocol::TMExportShares>(bytes);
                        if (!batch || batch->shares_size() == 0)
                            return f;
                        ++stats->reorderedShares;
                        stats->shareSendOrder.push_back(directBatchId(*batch));
                        // Node 0's frames sit; nodes 1 and 2 overtake them.
                        f.delay = (from == 0) ? 800ms : 50ms;
                        return f;
                    });
        }
        if (fault == Fault::slowObserver)
            net.lagAccept(observer, 5s);
        if (sluggish)
        {
            net.lagAccept(1, 2s);
            for (std::uint32_t to = 0; to <= observer; ++to)
                if (to != 1)
                    net.faultLink(1, to, [stats](std::uint16_t, std::size_t) {
                        ++stats->delayedValidatorTraffic;
                        SimFault f;
                        f.delay = 2500ms;
                        return f;
                    });
        }

        if (fault != Fault::none)
        {
            // Observe the impaired interval, then heal. These probes read
            // ledger cursors only; they do not alter node time or consensus.
            for (int second = 1; second < 16; ++second)
                net.in(std::chrono::seconds{second}, observer, [&net, stats]() {
                    auto const v = net.validSeq(observer);
                    auto const c = net.closedSeq(observer);
                    if (v > c)
                        stats->validatedAheadOfClosed =
                            std::max(stats->validatedAheadOfClosed, v - c);
                    for (std::uint32_t i = 0; i < observer; ++i)
                        if (net.validSeq(i) > v)
                            stats->validationGap = std::max(
                                stats->validationGap, net.validSeq(i) - v);
                });
            net.in(16s, observer, [&net, late, sluggish, fault]() {
                for (std::uint32_t id = 0; id <= observer; ++id)
                    net.clearLag(id);
                if (late)
                    for (std::uint32_t from = 0; from < observer; ++from)
                        net.faultLink(from, observer, {});
                if (sluggish)
                    for (std::uint32_t to = 0; to <= observer; ++to)
                        if (to != 1)
                            net.faultLink(1, to, {});
                if (fault == Fault::delayedRngReveals)
                    for (std::uint32_t from = 1; from < observer; ++from)
                        net.faultLink(from, 0, {});
                if (fault == Fault::reorderShares)
                    for (std::uint32_t from = 0; from < observer; ++from)
                        net.faultFrames(from, observer, {});
            });
        }
        auto const payment = world.submit(
            observer,
            jtx::pay(world.owner, world.destination, jtx::XRP(1000)),
            world.owner);
        if (!BEAST_EXPECT(payment->getResult() == tesSUCCESS))
            return std::nullopt;

        std::optional<uint256> origin;
        if (createIntent)
        {
            auto const tx = world.submit(observer, world.intent(), world.owner);
            if (!BEAST_EXPECT(tx->getResult() == tesSUCCESS))
                return std::nullopt;
            origin = tx->getID();
        }
        auto const target = warmLedger + 8;
        net.runTo(
            target,
            SteppingNetwork::RunBudget{1200, 1'000'000},
            SteppingNetwork::Cadence{(sluggish || gateTest) ? 250ms : 1000ms});
        net.controller().observeJobs({});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
            return std::nullopt;
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.validatedForkFree());
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);
        std::uint32_t localMismatches = 0;
        std::uint32_t localBuilds = 0;
        for (std::uint32_t i = 0; i <= observer; ++i)
            for (auto const& [seq, parent, hash] : stats->builds[i])
                if (seq <= target && parent == net.ledgerHash(0, seq - 1))
                {
                    ++localBuilds;
                    localMismatches += hash != net.ledgerHash(0, seq);
                }
        BEAST_EXPECT(localBuilds != 0);
        if (fault == Fault::none || fault == Fault::noDirectShares ||
            fault == Fault::noProposalShares ||
            fault == Fault::duplicateTraffic || fault == Fault::slowObserver ||
            fault == Fault::reorderShares)
            BEAST_EXPECT(localMismatches == 0);
        if (sluggish)
            BEAST_EXPECT(localMismatches != 0);
        std::map<std::uint32_t, uint256> validatedHistory;
        for (std::uint32_t i = 0; i <= observer; ++i)
        {
            std::uint32_t last = 0;
            for (auto const& sample : net.chainHistory(i))
            {
                BEAST_EXPECT(sample.validSeq >= last);
                last = sample.validSeq;
                if (sample.validSeq)
                {
                    auto const [it, inserted] = validatedHistory.emplace(
                        sample.validSeq, sample.validHash);
                    BEAST_EXPECT(inserted || it->second == sample.validHash);
                }
            }
        }
        if (fault == Fault::slowObserver)
            BEAST_EXPECT(stats->validatedAheadOfClosed != 0);
        if (late)
        {
            BEAST_EXPECT(stats->delayedValidations != 0);
            BEAST_EXPECT(stats->validationGap != 0);
        }
        if (fault == Fault::slowObserver || sluggish)
            BEAST_EXPECT(
                net.jobDiagnostics().find("queued:lagged JtAccept") !=
                std::string::npos);
        BEAST_EXPECT(world.observed->secrets[observer] == 0);
        BEAST_EXPECT(stats->ownReleases[observer] == 0);
        for (std::uint32_t i = 0; i < observer; ++i)
        {
            BEAST_EXPECT((world.observed->secrets[i] != 0) == rng);
            BEAST_EXPECT((stats->ownReleases[i] != 0) == createIntent);
            BEAST_EXPECT(stats->unauthorizedReleases[i] == 0);
        }
        if (fault == Fault::delayedRngReveals)
        {
            BEAST_EXPECT(stats->delayedReveals != 0);
            BEAST_EXPECT(stats->exportAheadOfRng != 0);
            BEAST_EXPECT(stats->gatedAccepts != 0);
        }
        if (fault == Fault::delayedExportCallbacks)
        {
            BEAST_EXPECT(stats->serviceLag >= 2);
            BEAST_EXPECT(stats->rngAheadOfExport != 0);
            BEAST_EXPECT(stats->gatedAccepts != 0);
            BEAST_EXPECT(
                net.jobDiagnostics().find("queued:lagged JtAdvance") !=
                std::string::npos);
        }

        bool entropySeen = false;
        bool partialEntropySeen = false;
        bool paymentSeen = false;
        bool witnessSeen = false;
        std::uint32_t witnesses = 0;
        std::vector<uint256> outcome;
        for (auto seq = warmLedger; seq <= target; ++seq)
        {
            auto const ledger = net.ledger(observer, seq);
            if (!BEAST_EXPECT(ledger != nullptr))
                return std::nullopt;
            for (std::uint32_t i = 0; i < observer; ++i)
                BEAST_EXPECT(net.ledgerHash(i, seq) == ledger->info().hash);
            for (auto const& [tx, meta] : ledger->txs)
            {
                BEAST_EXPECT(meta != nullptr);
                entropySeen |= tx->getTxnType() == ttCONSENSUS_ENTROPY;
                if (tx->getTxnType() == ttCONSENSUS_ENTROPY)
                    partialEntropySeen |=
                        tx->getFieldU16(sfEntropyCount) != observer;
                paymentSeen |= tx->getTransactionID() == payment->getID();
                witnessSeen |= tx->getTxnType() == ttEXPORT_SIGNATURES;
                witnesses += tx->getTxnType() == ttEXPORT_SIGNATURES;
                if (tx->getTxnType() == ttCONSENSUS_ENTROPY &&
                    seq > warmLedger && !sluggish)
                    BEAST_EXPECT(tx->getFieldU16(sfEntropyCount) == observer);
            }
            outcome.push_back(ledger->info().hash);
        }
        BEAST_EXPECT(entropySeen == rng);
        if (sluggish)
        {
            BEAST_EXPECT(partialEntropySeen);
            BEAST_EXPECT(stats->delayedValidatorTraffic != 0);
        }
        BEAST_EXPECT(paymentSeen);
        BEAST_EXPECT(witnessSeen == createIntent);
        BEAST_EXPECT(witnesses == (createIntent ? 1 : 0));
        if (fault == Fault::duplicateTraffic)
            BEAST_EXPECT(stats->duplicatedFrames != 0);
        if (fault == Fault::reorderShares)
        {
            BEAST_EXPECT(stats->reorderedShares >= 2);
            BEAST_EXPECT(
                stats->shareSendOrder.size() == stats->shareRecvOrder.size());
            std::map<uint256, std::vector<std::size_t>> recvAt;
            for (std::size_t i = 0; i < stats->shareRecvOrder.size(); ++i)
                recvAt[stats->shareRecvOrder[i]].push_back(i);
            std::map<uint256, std::size_t> consumed;
            std::vector<std::size_t> recvIndex(stats->shareSendOrder.size());
            for (std::size_t i = 0; i < stats->shareSendOrder.size(); ++i)
            {
                auto const& id = stats->shareSendOrder[i];
                auto const slot = consumed[id]++;
                if (!BEAST_EXPECT(slot < recvAt[id].size()))
                    break;
                recvIndex[i] = recvAt[id][slot];
            }
            for (std::size_t i = 0; i < recvIndex.size(); ++i)
                for (std::size_t j = i + 1; j < recvIndex.size(); ++j)
                    if (recvIndex[j] < recvIndex[i])
                        ++stats->invertedSharePairs;
            BEAST_EXPECT(stats->invertedSharePairs != 0);
            log << "  share-order: sent=" << stats->shareSendOrder.size()
                << " recv=" << stats->shareRecvOrder.size()
                << " inversions=" << stats->invertedSharePairs << std::endl;
        }
        if (origin)
        {
            auto const witnessSeq = witnessAt(net, *origin, warmLedger);
            BEAST_EXPECT(witnessSeq != 0);
            if (witnessSeq)
            {
                bool const builtWitness = stats->builds[observer].contains(
                    {witnessSeq,
                     net.ledgerHash(0, witnessSeq - 1),
                     net.ledgerHash(0, witnessSeq)});
                if (fault == Fault::noShares)
                    BEAST_EXPECT(!builtWitness);
                else if (!late)
                    BEAST_EXPECT(builtWitness);
            }
            auto const collected = net.node(observer)
                                       .app()
                                       .getConsensusExtensions()
                                       .postValidationExportSigCollector()
                                       .fullUnionSnapshot();
            auto const found = collected.find(*origin);
            if (fault == Fault::noDirectShares || fault == Fault::noShares)
            {
                BEAST_EXPECT(stats->droppedDirect != 0);
                BEAST_EXPECT(stats->directFrames[observer] == 0);
            }
            else
                BEAST_EXPECT(world.observed->directFrames[observer] != 0);
            if (fault == Fault::noProposalShares || fault == Fault::noShares)
            {
                BEAST_EXPECT(stats->droppedProposals != 0);
                BEAST_EXPECT(stats->proposalFrames[observer] == 0);
                BEAST_EXPECT(stats->exportRootFrames[observer] != 0);
            }
            else
                BEAST_EXPECT(stats->proposalFrames[observer] != 0);
            if (fault == Fault::noShares)
            {
                BEAST_EXPECT(found == collected.end());
                // Authority can overtake this observer before it finishes a
                // divergent build. Acquisition, not a mandatory JUMP, is the
                // recovery contract when both live evidence routes are absent.
                if (witnessSeq)
                    BEAST_EXPECT(stats->acquiredHashes[observer].contains(
                        net.ledgerHash(0, witnessSeq)));
            }
            else if (!late)
                BEAST_EXPECT(
                    found != collected.end() &&
                    found->second.size() == observer);
        }
        else
        {
            BEAST_EXPECT(world.observed->directFrames[observer] == 0);
            BEAST_EXPECT(world.observed->proposalFrames[observer] == 0);
        }
        log << "  behaviour: witness=" << witnessSeen
            << " entropy=" << entropySeen
            << " partialEntropy=" << partialEntropySeen
            << " directDropped=" << stats->droppedDirect
            << " proposalsDropped=" << stats->droppedProposals
            << " duplicated=" << stats->duplicatedFrames
            << " validationsDelayed=" << stats->delayedValidations
            << " validatorTrafficDelayed=" << stats->delayedValidatorTraffic
            << " validationGap=" << stats->validationGap
            << " validatedAhead=" << stats->validatedAheadOfClosed
            << " localBuilds=" << localBuilds
            << " localMismatches=" << localMismatches
            << " observerJumps=" << net.closedJumps(observer).size()
            << " exportAhead=" << stats->exportAheadOfRng
            << " rngAhead=" << stats->rngAheadOfExport
            << " serviceLag=" << stats->serviceLag << std::endl;
        outcome.push_back(sha512Half(
            stats->droppedDirect,
            stats->droppedProposals,
            stats->duplicatedFrames,
            stats->delayedValidations,
            stats->delayedValidatorTraffic,
            stats->validationGap,
            stats->validatedAheadOfClosed,
            localBuilds,
            localMismatches,
            stats->exportAheadOfRng,
            stats->rngAheadOfExport,
            stats->serviceLag,
            stats->invertedSharePairs,
            stats->shareSendOrder.size(),
            stats->shareRecvOrder.size()));
        return outcome;
    }

    std::optional<std::vector<uint256>>
    overlappingOrigins(SteppingNetwork& net)
    {
        World world(net, false, true);
        if (!ready(world))
            return std::nullopt;
        jtx::Account owner2{"dsf-export-owner-2"};
        auto const fund = [&](jtx::Account const& acct) {
            auto const tx = world.submit(
                observer,
                jtx::pay(jtx::Account::master, acct, jtx::XRP(10'000)),
                jtx::Account::master);
            return tx && tx->getResult() == tesSUCCESS;
        };
        if (!BEAST_EXPECT(fund(world.owner) && fund(owner2)))
            return std::nullopt;
        net.runTo(warmLedger + 2);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger + 2))
            return std::nullopt;

        auto const open =
            net.node(observer).app().openLedger().current()->seq();
        auto const longWindow = open + ExportLimits::maxAdmissionWindowLedgers;
        auto const shortWindow = open + 3;
        std::vector<uint256> origins;
        auto const submitIntent = [&](jtx::Account const& acct,
                                      std::uint32_t ticket,
                                      std::uint32_t lastLedger) {
            auto const tx = world.submit(
                observer, world.intent(acct, ticket, lastLedger), acct);
            if (!BEAST_EXPECT(tx && tx->getResult() == tesSUCCESS))
                return false;
            origins.push_back(tx->getID());
            return true;
        };
        if (!submitIntent(world.owner, 1, longWindow) ||
            !submitIntent(world.owner, 2, shortWindow) ||
            !submitIntent(owner2, 1, longWindow))
            return std::nullopt;

        auto const target = warmLedger + 12;
        net.runTo(target, SteppingNetwork::RunBudget{1600, 1'200'000});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
            return std::nullopt;
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.validatedForkFree());
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(world.observed->secrets[observer] == 0);
        BEAST_EXPECT(world.observed->ownReleases[observer] == 0);
        for (std::uint32_t i = 0; i < observer; ++i)
        {
            BEAST_EXPECT(world.observed->ownReleases[i] != 0);
            BEAST_EXPECT(world.observed->unauthorizedReleases[i] == 0);
        }
        auto const collected = net.node(observer)
                                   .app()
                                   .getConsensusExtensions()
                                   .postValidationExportSigCollector()
                                   .fullUnionSnapshot();
        BEAST_EXPECT(collected.size() == origins.size());
        std::map<uint256, std::uint32_t> witnessHits;
        std::set<uint256> unexpected;
        std::vector<uint256> outcome;
        for (auto seq = warmLedger; seq <= target; ++seq)
        {
            auto const canonical = net.ledger(0, seq);
            if (!BEAST_EXPECT(canonical != nullptr))
                return std::nullopt;
            for (std::uint32_t i = 1; i <= observer; ++i)
            {
                auto const ledger = net.ledger(i, seq);
                if (!BEAST_EXPECT(ledger != nullptr))
                    return std::nullopt;
                BEAST_EXPECT(ledger->info().hash == canonical->info().hash);
            }
            outcome.push_back(canonical->info().hash);
            for (auto const& [tx, meta] : canonical->txs)
            {
                if (tx->getTxnType() != ttEXPORT_SIGNATURES)
                    continue;
                if (!BEAST_EXPECT(meta != nullptr))
                    return std::nullopt;
                auto const origin = tx->getFieldH256(sfTransactionHash);
                auto const expected =
                    std::find(origins.begin(), origins.end(), origin);
                if (expected == origins.end())
                    unexpected.insert(origin);
                else
                    ++witnessHits[origin];
                auto const txBytes = tx->getSerializer().getData();
                auto const metaBytes = meta->getSerializer().getData();
                outcome.push_back(
                    sha512Half(makeSlice(txBytes), makeSlice(metaBytes)));
                for (std::uint32_t i = 0; i <= observer; ++i)
                {
                    auto const ledger = net.ledger(i, seq);
                    bool found = false;
                    for (auto const& [peerTx, peerMeta] : ledger->txs)
                    {
                        if (peerTx->getTxnType() != ttEXPORT_SIGNATURES ||
                            peerTx->getFieldH256(sfTransactionHash) != origin)
                            continue;
                        found = true;
                        BEAST_EXPECT(
                            peerTx->getSerializer().getData() == txBytes);
                        BEAST_EXPECT(
                            peerMeta != nullptr &&
                            peerMeta->getSerializer().getData() == metaBytes);
                    }
                    BEAST_EXPECT(found);
                }
            }
        }
        BEAST_EXPECT(unexpected.empty());
        for (auto const& origin : origins)
        {
            auto const found = collected.find(origin);
            if (!BEAST_EXPECT(found != collected.end()))
                return std::nullopt;
            BEAST_EXPECT(found->second.size() == observer);
            BEAST_EXPECT(witnessHits[origin] == 1);
            auto const seq = witnessAt(net, origin, warmLedger);
            BEAST_EXPECT(seq != 0);
            if (seq)
                BEAST_EXPECT(world.observed->builds[observer].contains(
                    {seq, net.ledgerHash(0, seq - 1), net.ledgerHash(0, seq)}));
        }
        outcome.insert(outcome.end(), origins.begin(), origins.end());
        return outcome;
    }

    std::optional<std::vector<uint256>>
    incompleteOriginInCandidate(SteppingNetwork& net)
    {
        World world(net, false, true);
        if (!ready(world))
            return std::nullopt;
        auto const fund = world.submit(
            observer,
            jtx::pay(jtx::Account::master, world.owner, jtx::XRP(10'000)),
            jtx::Account::master);
        if (!BEAST_EXPECT(fund && fund->getResult() == tesSUCCESS))
            return std::nullopt;
        net.runTo(warmLedger + 2);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger + 2))
            return std::nullopt;

        auto const open =
            net.node(observer).app().openLedger().current()->seq();
        auto const window = open + ExportLimits::maxAdmissionWindowLedgers;
        auto const txA = world.submit(
            observer,
            world.intent(world.owner, 1, window, {0, 1}),
            world.owner);
        auto const txB = world.submit(
            observer,
            world.intent(world.owner, 2, window, {0, 1, 2}),
            world.owner);
        if (!BEAST_EXPECT(
                txA && txA->getResult() == tesSUCCESS && txB &&
                txB->getResult() == tesSUCCESS))
            return std::nullopt;
        auto const originA = txA->getID();
        auto const originB = txB->getID();
        auto const qA = ExportLimits::committeeQuorumThreshold(2);
        auto const qB = ExportLimits::committeeQuorumThreshold(3);
        auto const stats = world.observed;
        auto const& v2keys = net.node(2).app().getValidatorKeys();
        auto const v2sign = v2keys.keys->publicKey;
        auto const v2master = v2keys.keys->masterPublicKey;
        auto const starveBfromV2 = [stats, originB, v2sign, v2master](
                                       std::uint16_t type,
                                       SimPipe::Frame bytes) {
            SimFault f;
            auto const hit = [&](std::string const& blob) {
                auto const share = ExportShare::parse(makeSlice(blob));
                return share && share->originTxn == originB &&
                    (share->signingKey == v2sign ||
                     share->signingKey == v2master);
            };
            if (type == protocol::mtEXPORT_SHARES)
            {
                auto const batch = decodeFrame<protocol::TMExportShares>(bytes);
                for (auto const& share : batch->shares())
                    if (hit(share))
                    {
                        ++stats->droppedStarvedDirect;
                        f.drop = true;
                        return f;
                    }
            }
            if (type == protocol::mtPROPOSE_LEDGER)
            {
                auto const proposal =
                    decodeFrame<protocol::TMProposeSet>(bytes);
                for (auto const& share : proposal->exportsignatures())
                    if (hit(share))
                    {
                        ++stats->droppedStarvedProposals;
                        f.drop = true;
                        return f;
                    }
            }
            return f;
        };
        for (std::uint32_t from = 0; from < observer; ++from)
            net.faultFrames(from, observer, starveBfromV2);
        net.controller().observeJobs([&net, stats, originA, originB](
                                         std::uint32_t id,
                                         JobType,
                                         std::string const&) {
            if (id != observer || !net.isLive(id) || stats->sawPartialCandidate)
                return;
            auto& ce = net.node(id).app().getConsensusExtensions();
            if (!ce.exportSigSetMap_)
                return;
            stats->sawObserverMap = true;
            auto const leavesA =
                originSidecarLeaves(*ce.exportSigSetMap_, originA);
            auto const leavesB =
                originSidecarLeaves(*ce.exportSigSetMap_, originB);
            if (!leavesA || !leavesB)
                return;
            stats->candidateLeavesA = *leavesA;
            stats->candidateLeavesB = *leavesB;
            if (!ce.roundParentLedger_)
                return;
            auto const& parent = *ce.roundParentLedger_;
            auto const pending = ce.pendingRoundExports(parent.info().seq + 1);
            auto const needA = ExportLimits::committeeQuorumThreshold(2);
            auto const needB = ExportLimits::committeeQuorumThreshold(3);
            if (pending.contains(originA) && pending.contains(originB) &&
                *leavesA == needA && *leavesB > 0 && *leavesB < needB)
                stats->sawPartialCandidate = true;
        });

        auto const mid = warmLedger + 8;
        net.runTo(mid, SteppingNetwork::RunBudget{1600, 1'200'000});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= mid))
            return std::nullopt;
        BEAST_EXPECT(stats->droppedStarvedDirect != 0);
        BEAST_EXPECT(stats->droppedStarvedProposals != 0);
        BEAST_EXPECT(stats->sawPartialCandidate);
        BEAST_EXPECT(stats->candidateLeavesA == qA);
        BEAST_EXPECT(stats->candidateLeavesB > 0);
        BEAST_EXPECT(stats->candidateLeavesB < qB);
        log << "  candidate: A=" << stats->candidateLeavesA << "/" << qA
            << " B=" << stats->candidateLeavesB << "/" << qB
            << " sawMap=" << stats->sawObserverMap
            << " sawPartial=" << stats->sawPartialCandidate
            << " droppedDirect=" << stats->droppedStarvedDirect
            << " droppedProposals=" << stats->droppedStarvedProposals
            << std::endl;
        net.controller().observeJobs({});

        for (std::uint32_t from = 0; from < observer; ++from)
            net.faultFrames(from, observer, {});
        auto const target = warmLedger + 12;
        net.runTo(target, SteppingNetwork::RunBudget{1600, 1'200'000});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
            return std::nullopt;
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.validatedForkFree());
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(stats->secrets[observer] == 0);
        BEAST_EXPECT(stats->ownReleases[observer] == 0);
        auto const seqA = witnessAt(net, originA, warmLedger);
        BEAST_EXPECT(seqA != 0);
        // Observer may omit locally and acquire the authoritative A witness.
        std::map<uint256, std::uint32_t> hits;
        std::set<uint256> unexpected;
        std::vector<uint256> outcome;
        for (auto seq = warmLedger; seq <= target; ++seq)
        {
            auto const canonical = net.ledger(0, seq);
            if (!BEAST_EXPECT(canonical != nullptr))
                return std::nullopt;
            for (std::uint32_t i = 1; i <= observer; ++i)
            {
                auto const ledger = net.ledger(i, seq);
                if (!BEAST_EXPECT(ledger != nullptr))
                    return std::nullopt;
                BEAST_EXPECT(ledger->info().hash == canonical->info().hash);
            }
            outcome.push_back(canonical->info().hash);
            for (auto const& [tx, meta] : canonical->txs)
            {
                if (tx->getTxnType() != ttEXPORT_SIGNATURES)
                    continue;
                if (!BEAST_EXPECT(meta != nullptr))
                    return std::nullopt;
                auto const origin = tx->getFieldH256(sfTransactionHash);
                if (origin != originA && origin != originB)
                    unexpected.insert(origin);
                else
                    ++hits[origin];
                auto const txBytes = tx->getSerializer().getData();
                auto const metaBytes = meta->getSerializer().getData();
                outcome.push_back(
                    sha512Half(makeSlice(txBytes), makeSlice(metaBytes)));
                for (std::uint32_t i = 0; i <= observer; ++i)
                {
                    auto const ledger = net.ledger(i, seq);
                    bool found = false;
                    for (auto const& [peerTx, peerMeta] : ledger->txs)
                    {
                        if (peerTx->getTxnType() != ttEXPORT_SIGNATURES ||
                            peerTx->getFieldH256(sfTransactionHash) != origin)
                            continue;
                        found = true;
                        BEAST_EXPECT(
                            peerTx->getSerializer().getData() == txBytes);
                        BEAST_EXPECT(
                            peerMeta != nullptr &&
                            peerMeta->getSerializer().getData() == metaBytes);
                    }
                    BEAST_EXPECT(found);
                }
            }
        }
        BEAST_EXPECT(unexpected.empty());
        BEAST_EXPECT(hits[originA] == 1);
        // B may be witnessed by the informed cohort; observer may acquire.
        BEAST_EXPECT(hits[originB] <= 1);
        outcome.push_back(originA);
        outcome.push_back(originB);
        outcome.push_back(sha512Half(
            static_cast<std::uint32_t>(stats->candidateLeavesA),
            static_cast<std::uint32_t>(stats->candidateLeavesB)));
        return outcome;
    }

    std::optional<std::vector<uint256>>
    observerRestartAcrossExport(SteppingNetwork& net)
    {
        using namespace std::chrono_literals;
        World world(net, true, true);
        if (!ready(world))
            return std::nullopt;
        auto const funding = world.submit(
            observer,
            jtx::pay(jtx::Account::master, world.owner, jtx::XRP(10'000)),
            jtx::Account::master);
        if (!BEAST_EXPECT(funding && funding->getResult() == tesSUCCESS))
            return std::nullopt;
        net.runTo(warmLedger + 2);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger + 2))
            return std::nullopt;

        auto const stats = world.observed;
        net.controller().setJobLag(
            observer, jtADVANCE, "validatedLedgerWork", 20s);
        net.controller().setJobLag(
            observer, jtEXPORT_SHARES, "recvExportShares", 20s);

        auto const payment = world.submit(
            observer,
            jtx::pay(world.owner, world.destination, jtx::XRP(1000)),
            world.owner);
        if (!BEAST_EXPECT(payment && payment->getResult() == tesSUCCESS))
            return std::nullopt;
        auto const tx = world.submit(observer, world.intent(), world.owner);
        if (!BEAST_EXPECT(tx && tx->getResult() == tesSUCCESS))
            return std::nullopt;
        auto const origin = tx->getID();
        net.runTo(warmLedger + 6);
        BEAST_EXPECT(!net.node(observer).app().getValidatorKeys().keys);
        BEAST_EXPECT(stats->ownReleases[observer] == 0);
        BEAST_EXPECT(stats->secrets[observer] == 0);
        auto const preSeq = net.validSeq(observer);
        auto const preHash = net.ledgerHash(observer, preSeq);
        auto const replayBefore =
            net.node(observer)
                .app()
                .getConsensusExtensions()
                .lastExportReplaySeq_.load(std::memory_order_relaxed);
        auto const originWitnessed = witnessAt(net, origin, warmLedger, 0);
        auto const pendingValidator = net.node(0)
                                          .app()
                                          .getConsensusExtensions()
                                          .hasEligiblePendingExports();
        if (!BEAST_EXPECT(originWitnessed != 0 || pendingValidator))
        {
            log << "  no validator pending-origin or witness before stop"
                << " originWitnessed=" << originWitnessed
                << " pendingValidator=" << pendingValidator << std::endl;
            return std::nullopt;
        }

        auto const staleHorizon = net.controller().now() + 25s;
        net.at(staleHorizon, observer, [stats]() {
            stats->oldGenerationRan = true;
        });
        net.isolateNodeAndFlush(observer);
        auto const laggedVlw = net.controller().laggedPendingJobCount(
            observer, jtADVANCE, "validatedLedgerWork");
        stats->queuedExportWork = laggedVlw > 0;
        if (!BEAST_EXPECT(stats->queuedExportWork))
        {
            log << "  no current lagged validatedLedgerWork after isolate/flush"
                << " vlw=" << laggedVlw << " " << net.jobDiagnostics()
                << std::endl;
            return std::nullopt;
        }
        net.stopNode(observer);
        BEAST_EXPECT(!net.isLive(observer));
        auto const seqBeforeStop = net.validSeq(0);
        auto const open = net.node(0).app().openLedger().current()->seq();
        auto const gapTx = world.submit(
            0,
            world.intent(
                world.owner,
                2,
                open + ExportLimits::maxAdmissionWindowLedgers,
                {0, 1, 2}),
            world.owner);
        if (!BEAST_EXPECT(gapTx && gapTx->getResult() == tesSUCCESS))
            return std::nullopt;
        auto const originGap = gapTx->getID();
        net.runOnly({0, 1, 2}, seqBeforeStop + 1);
        auto const pendingGap = net.node(0)
                                    .app()
                                    .getConsensusExtensions()
                                    .hasEligiblePendingExports();
        if (!BEAST_EXPECT(pendingGap))
        {
            log << "  gap origin not pending on a live validator after +1"
                << std::endl;
            return std::nullopt;
        }
        net.runOnly({0, 1, 2}, seqBeforeStop + 3);
        BEAST_EXPECT(net.validSeq(0) > seqBeforeStop);
        BEAST_EXPECT(net.validSeq(1) > seqBeforeStop);
        BEAST_EXPECT(net.validSeq(2) > seqBeforeStop);
        auto const gapWitness = witnessAt(net, originGap, seqBeforeStop, 0);
        if (!BEAST_EXPECT(gapWitness > seqBeforeStop))
            return std::nullopt;
        BEAST_EXPECT(net.ledgersAgree(seqBeforeStop));
        BEAST_EXPECT(net.ledgersAgree(gapWitness));
        BEAST_EXPECT(net.validatedForkFree());
        for (std::uint32_t n = 0; n < observer; ++n)
            BEAST_EXPECT(
                witnessAt(net, originGap, seqBeforeStop, n) == gapWitness);
        BEAST_EXPECT(net.controller().now() < staleHorizon);

        net.restartNode(observer);
        if (!BEAST_EXPECT(net.isLive(observer)))
            return std::nullopt;
        BEAST_EXPECT(!net.node(observer).app().getValidatorKeys().keys);
        net.reconnectNode(observer);
        net.clearLag(observer);
        BEAST_EXPECT(net.controller().now() < staleHorizon);
        net.at(net.controller().now() + 1s, observer, [stats]() {
            stats->newGenerationRan = true;
        });

        auto const target = net.validSeq(0) + 4;
        net.runTo(target, SteppingNetwork::RunBudget{1600, 1'200'000});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
            return std::nullopt;
        if (net.controller().now() <= staleHorizon)
            net.settle((staleHorizon + 1s) - net.controller().now(), 1'200'000);
        BEAST_EXPECT(net.controller().now() > staleHorizon);
        BEAST_EXPECT(stats->newGenerationRan);
        BEAST_EXPECT(!stats->oldGenerationRan);
        BEAST_EXPECT(net.ledgerHash(0, preSeq) == preHash);
        BEAST_EXPECT(net.ledgerHash(observer, preSeq) == preHash);
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.validatedForkFree());
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);
        BEAST_EXPECT(stats->secrets[observer] == 0);
        BEAST_EXPECT(stats->ownReleases[observer] == 0);
        auto const seqA = witnessAt(net, origin, warmLedger);
        BEAST_EXPECT(seqA != 0);
        BEAST_EXPECT(witnessAt(net, originGap, seqBeforeStop) == gapWitness);
        auto const replayAfter =
            net.node(observer)
                .app()
                .getConsensusExtensions()
                .lastExportReplaySeq_.load(std::memory_order_relaxed);
        BEAST_EXPECT(replayAfter >= gapWitness);
        BEAST_EXPECT(replayAfter >= replayBefore);
        log << "  restart: preSeq=" << preSeq << " gapWitness=" << gapWitness
            << " laggedVlw=" << laggedVlw
            << " originWitnessed=" << originWitnessed
            << " pendingValidator=" << pendingValidator
            << " replay=" << replayBefore << "->" << replayAfter << std::endl;
        std::map<uint256, std::uint32_t> hits;
        std::set<uint256> unexpected;
        std::vector<uint256> outcome;
        for (auto seq = warmLedger; seq <= target; ++seq)
        {
            auto const canonical = net.ledger(0, seq);
            if (!BEAST_EXPECT(canonical != nullptr))
                return std::nullopt;
            for (std::uint32_t i = 1; i <= observer; ++i)
            {
                auto const ledger = net.ledger(i, seq);
                if (!BEAST_EXPECT(ledger != nullptr))
                    return std::nullopt;
                BEAST_EXPECT(ledger->info().hash == canonical->info().hash);
            }
            outcome.push_back(canonical->info().hash);
            for (auto const& [tx, meta] : canonical->txs)
            {
                if (tx->getTxnType() != ttEXPORT_SIGNATURES)
                    continue;
                if (!BEAST_EXPECT(meta != nullptr))
                    return std::nullopt;
                auto const id = tx->getFieldH256(sfTransactionHash);
                if (id != origin && id != originGap)
                    unexpected.insert(id);
                else
                    ++hits[id];
                auto const txBytes = tx->getSerializer().getData();
                auto const metaBytes = meta->getSerializer().getData();
                outcome.push_back(
                    sha512Half(makeSlice(txBytes), makeSlice(metaBytes)));
                for (std::uint32_t i = 0; i <= observer; ++i)
                {
                    auto const ledger = net.ledger(i, seq);
                    bool found = false;
                    for (auto const& [peerTx, peerMeta] : ledger->txs)
                    {
                        if (peerTx->getTxnType() != ttEXPORT_SIGNATURES ||
                            peerTx->getFieldH256(sfTransactionHash) != id)
                            continue;
                        found = true;
                        BEAST_EXPECT(
                            peerTx->getSerializer().getData() == txBytes);
                        BEAST_EXPECT(
                            peerMeta != nullptr &&
                            peerMeta->getSerializer().getData() == metaBytes);
                    }
                    BEAST_EXPECT(found);
                }
            }
        }
        BEAST_EXPECT(unexpected.empty());
        BEAST_EXPECT(hits[origin] <= 1);
        BEAST_EXPECT(hits[originGap] == 1);
        outcome.push_back(origin);
        outcome.push_back(originGap);
        return outcome;
    }

    std::optional<std::vector<uint256>>
    invalidShareDoesNotPoison(SteppingNetwork& net)
    {
        using namespace std::chrono_literals;
        World world(net, false, true);
        if (!ready(world))
            return std::nullopt;

        test::StreamSink sink{beast::severities::kTrace};
        auto& observerCE = net.node(observer).app().getConsensusExtensions();
        auto const previousJournal = observerCE.j_;
        observerCE.j_ = beast::Journal{sink};
        scope_exit restoreJournal{[&observerCE, previousJournal]() {
            observerCE.j_ = previousJournal;
        }};

        auto const funding = world.submit(
            observer,
            jtx::pay(jtx::Account::master, world.owner, jtx::XRP(10'000)),
            jtx::Account::master);
        if (!BEAST_EXPECT(funding && funding->getResult() == tesSUCCESS))
            return std::nullopt;
        net.runTo(warmLedger + 2);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger + 2))
            return std::nullopt;

        auto const stats = world.observed;
        auto const tx = world.submit(observer, world.intent(), world.owner);
        if (!BEAST_EXPECT(tx && tx->getResult() == tesSUCCESS))
            return std::nullopt;
        auto const origin = tx->getID();
        stats->captureOrigin = origin;

        auto collectorHasHonest = [&]() {
            if (!stats->honestShare)
                return false;
            auto const snap = observerCE.postValidationExportSigCollector()
                                  .fullUnionSnapshot();
            auto const it = snap.find(origin);
            if (it == snap.end())
                return false;
            for (auto const& contrib : it->second)
            {
                if (contrib.signingKey == stats->honestShare->signingKey &&
                    contrib.position == stats->honestShare->committeePosition &&
                    contrib.signature.size() ==
                        stats->honestShare->signature.size() &&
                    std::equal(
                        contrib.signature.data(),
                        contrib.signature.data() + contrib.signature.size(),
                        stats->honestShare->signature.data()))
                    return true;
            }
            return false;
        };

        auto observerOriginLive = [&]() {
            if (!stats->honestShare)
                return false;
            auto const& share = *stats->honestShare;
            if (net.validSeq(observer) < share.originLedgerSeq)
                return false;
            if (net.ledgerHash(observer, share.originLedgerSeq) !=
                share.originLedgerHash)
                return false;
            auto const ledger = net.ledger(observer, net.validSeq(observer));
            if (!ledger)
                return false;
            auto const latch =
                ledger->read(keylet::exportLatch(share.owner, share.originTxn));
            return latch && latch->getType() == ltEXPORT_LATCH &&
                !latch->isFieldPresent(sfExportSignatureHash) &&
                latch->getFieldH256(sfTransactionHash) == share.originTxn &&
                ledger->seq() <= latch->getFieldU32(sfLastLedgerSequence);
        };

        auto const junk = generateKeyPair(
            KeyType::secp256k1, generateSeed("dsf-b7-invalid-share"));
        auto const invalidResult = std::to_string(
            static_cast<unsigned>(ExportSigCollector::AdmitResult::invalid));
        std::size_t sent = 0;
        bool injected = false;
        std::size_t captureMark = 0;
        uint256 badWire{};
        net.controller().observeJobs([&](std::uint32_t,
                                         JobType,
                                         std::string const&) {
            if (injected)
                return;
            if (!stats->honestShare || !collectorHasHonest() ||
                !observerOriginLive())
                return;
            auto bad = *stats->honestShare;
            bad.signature =
                sign(junk.first, junk.second, Slice{"dsf-b7-not-payload", 18});
            if (!bad.validShape())
                return;
            stats->badSignature = bad.signature;
            badWire = bad.wireHash();
            auto const framed = bad.serialize();
            stats->badShareBytes.assign(
                reinterpret_cast<char const*>(framed.data()), framed.size());
            protocol::TMExportShares batch;
            batch.add_shares(framed.data(), framed.size());
            auto const msg =
                std::make_shared<Message>(batch, protocol::mtEXPORT_SHARES);
            auto const targetPub =
                net.node(observer).app().nodeIdentity().first;
            for (auto const& peer :
                 net.node(0).app().overlay().getActivePeers())
            {
                if (peer->getNodePublic() != targetPub)
                    continue;
                peer->send(msg);
                ++sent;
                break;
            }
            captureMark = sink.messages().str().size();
            stats->sawBadWhilePending = observerOriginLive();
            injected = true;
        });
        net.runTo(warmLedger + 8, SteppingNetwork::RunBudget{1600, 1'200'000});
        net.controller().observeJobs({});
        if (!BEAST_EXPECT(injected && stats->honestShare && sent == 1))
        {
            log << "  no observer-admitted pending origin before inject"
                << " captured=" << static_cast<bool>(stats->honestShare)
                << " admitted=" << collectorHasHonest()
                << " live=" << observerOriginLive() << " injected=" << injected
                << " sent=" << sent << std::endl;
            return std::nullopt;
        }
        if (!BEAST_EXPECT(stats->sawBadWhilePending))
            return std::nullopt;
        if (!BEAST_EXPECT(stats->badFramesReceived == 1))
        {
            log << "  observer did not receive exactly one forged frame"
                << " received=" << stats->badFramesReceived << " sent=" << sent
                << std::endl;
            return std::nullopt;
        }

        auto const captured = sink.messages().str().substr(captureMark);
        auto const originText = "origin=" + to_string(origin);
        auto const wireText = "wire=" + to_string(badWire);
        auto const commitNeedle = std::string{"ExportShare: collector commit"};
        bool sawRejection = false;
        {
            std::istringstream in{captured};
            for (std::string line; std::getline(in, line);)
            {
                if (line.find(commitNeedle) == std::string::npos)
                    continue;
                if (line.find(originText) == std::string::npos)
                    continue;
                if (line.find(wireText) == std::string::npos)
                    continue;
                if (line.find("signatureVerified=false") == std::string::npos)
                    continue;
                if (line.find("result=" + invalidResult) == std::string::npos)
                    continue;
                sawRejection = true;
                break;
            }
        }
        if (!BEAST_EXPECT(sawRejection))
        {
            log << "  no observer collector-commit rejection for forged wire"
                << " " << originText << " " << wireText
                << " result=" << invalidResult << std::endl;
            std::istringstream in{captured};
            for (std::string line; std::getline(in, line);)
            {
                if (line.find("ExportShare:") == std::string::npos)
                    continue;
                if (line.find(originText) == std::string::npos &&
                    line.find(wireText) == std::string::npos)
                    continue;
                log << "    " << line << std::endl;
            }
            return std::nullopt;
        }

        BEAST_EXPECT(collectorHasHonest());
        BEAST_EXPECT(
            observerCE.postValidationExportSigCollector().positionStatus(
                origin, stats->honestShare->committeePosition) ==
            ExportSigCollector::PositionStatus::unique);
        auto const snapAfter =
            observerCE.postValidationExportSigCollector().fullUnionSnapshot();
        auto const foundAfter = snapAfter.find(origin);
        if (!BEAST_EXPECT(foundAfter != snapAfter.end()))
            return std::nullopt;
        for (auto const& contrib : foundAfter->second)
            BEAST_EXPECT(
                contrib.signature.size() != stats->badSignature.size() ||
                !std::equal(
                    contrib.signature.data(),
                    contrib.signature.data() + contrib.signature.size(),
                    stats->badSignature.data()));

        auto const target = net.validSeq(0) + 4;
        net.runTo(target, SteppingNetwork::RunBudget{1600, 1'200'000});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
            return std::nullopt;
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.validatedForkFree());
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);
        BEAST_EXPECT(!net.node(observer).app().getValidatorKeys().keys);
        BEAST_EXPECT(stats->secrets[observer] == 0);
        BEAST_EXPECT(stats->ownReleases[observer] == 0);
        auto const seqW = witnessAt(net, origin, warmLedger);
        if (!BEAST_EXPECT(seqW != 0))
            return std::nullopt;
        BEAST_EXPECT(stats->builds[observer].contains(
            {seqW, net.ledgerHash(0, seqW - 1), net.ledgerHash(0, seqW)}));
        BEAST_EXPECT(collectorHasHonest());
        BEAST_EXPECT(
            observerCE.postValidationExportSigCollector().positionStatus(
                origin, stats->honestShare->committeePosition) !=
            ExportSigCollector::PositionStatus::conflicted);

        std::map<uint256, std::uint32_t> hits;
        std::set<uint256> unexpected;
        std::vector<uint256> outcome;
        for (auto seq = warmLedger; seq <= target; ++seq)
        {
            auto const canonical = net.ledger(0, seq);
            if (!BEAST_EXPECT(canonical != nullptr))
                return std::nullopt;
            for (std::uint32_t i = 1; i <= observer; ++i)
            {
                auto const ledger = net.ledger(i, seq);
                if (!BEAST_EXPECT(ledger != nullptr))
                    return std::nullopt;
                BEAST_EXPECT(ledger->info().hash == canonical->info().hash);
            }
            outcome.push_back(canonical->info().hash);
            for (auto const& [wtx, meta] : canonical->txs)
            {
                if (wtx->getTxnType() != ttEXPORT_SIGNATURES)
                    continue;
                if (!BEAST_EXPECT(meta != nullptr))
                    return std::nullopt;
                auto const id = wtx->getFieldH256(sfTransactionHash);
                if (id != origin)
                    unexpected.insert(id);
                else
                    ++hits[id];
                auto const txBytes = wtx->getSerializer().getData();
                auto const metaBytes = meta->getSerializer().getData();
                outcome.push_back(
                    sha512Half(makeSlice(txBytes), makeSlice(metaBytes)));
                for (std::uint32_t i = 0; i <= observer; ++i)
                {
                    auto const ledger = net.ledger(i, seq);
                    bool found = false;
                    for (auto const& [peerTx, peerMeta] : ledger->txs)
                    {
                        if (peerTx->getTxnType() != ttEXPORT_SIGNATURES ||
                            peerTx->getFieldH256(sfTransactionHash) != id)
                            continue;
                        found = true;
                        BEAST_EXPECT(
                            peerTx->getSerializer().getData() == txBytes);
                        BEAST_EXPECT(
                            peerMeta != nullptr &&
                            peerMeta->getSerializer().getData() == metaBytes);
                    }
                    BEAST_EXPECT(found);
                }
            }
        }
        BEAST_EXPECT(unexpected.empty());
        BEAST_EXPECT(hits[origin] == 1);
        log << "  invalid-share: received=" << stats->badFramesReceived
            << " sent=" << sent
            << " pendingAtInject=" << stats->sawBadWhilePending
            << " rejected=" << sawRejection << " witness=" << seqW << std::endl;
        outcome.push_back(origin);
        outcome.push_back(badWire);
        return outcome;
    }

    std::optional<std::vector<uint256>>
    noQuorumPartitionHeal(SteppingNetwork& net)
    {
        using namespace std::chrono_literals;
        World world(net, true, true);
        if (!ready(world))
            return std::nullopt;
        auto const stats = world.observed;
        auto const [quorum, trusted] =
            net.node(0).app().validators().getQuorumKeys();
        auto const remaining = observer - 1;
        if (!BEAST_EXPECT(quorum > remaining))
        {
            log << "  remaining validators would still meet quorum"
                << " quorum=" << quorum << " remaining=" << remaining
                << " trusted=" << trusted.size() << std::endl;
            return std::nullopt;
        }
        auto const funding = world.submit(
            observer,
            jtx::pay(jtx::Account::master, world.owner, jtx::XRP(10'000)),
            jtx::Account::master);
        if (!BEAST_EXPECT(funding && funding->getResult() == tesSUCCESS))
            return std::nullopt;
        net.runTo(warmLedger + 2);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger + 2))
            return std::nullopt;

        auto originReleases = [&](uint256 const& origin) {
            auto const it = stats->originOwnReleases.find(origin);
            if (it == stats->originOwnReleases.end())
                return std::size_t{0};
            std::size_t n = 0;
            for (auto const c : it->second)
                n += c;
            return n;
        };

        auto const stallValid = net.minValidatedSeq();
        auto const stallHash = net.ledgerHash(0, stallValid);
        net.isolateNodeAndFlush(2);
        if (!BEAST_EXPECT(net.runUntil(
                [&] {
                    return net.closedSeq(0) > stallValid &&
                        net.validSeq(0) <= stallValid &&
                        net.validSeq(1) <= stallValid &&
                        net.validSeq(observer) <= stallValid;
                },
                SteppingNetwork::RunBudget{20, 1'200'000})))
        {
            log << "  survivors did not close past stalled validation"
                << " closed=" << net.closedSeq(0)
                << " valid=" << net.validSeq(0) << std::endl;
            return std::nullopt;
        }
        BEAST_EXPECT(net.mode(2) == OperatingMode::DISCONNECTED);
        for (std::uint32_t n = 0; n <= observer; ++n)
            BEAST_EXPECT(net.validSeq(n) <= stallValid);
        BEAST_EXPECT(net.closedSeq(1) == net.closedSeq(0));

        auto const open = net.node(0).app().openLedger().current()->seq();
        auto const gapTx = world.submit(
            0,
            world.intent(
                world.owner, 1, open + ExportLimits::maxAdmissionWindowLedgers),
            world.owner);
        if (!BEAST_EXPECT(gapTx && gapTx->getResult() == tesSUCCESS))
            return std::nullopt;
        auto const origin = gapTx->getID();
        auto const releasesAtSubmit = originReleases(origin);
        auto originClosedOn0 = [&] {
            for (auto seq = stallValid + 1; seq <= net.closedSeq(0); ++seq)
                if (ledgerHasTx(net.ledger(0, seq), origin))
                    return true;
            return false;
        };
        if (!BEAST_EXPECT(net.runUntil(
                [&] {
                    return originClosedOn0() && net.validSeq(0) <= stallValid;
                },
                SteppingNetwork::RunBudget{20, 1'200'000})))
        {
            log << "  speculative origin missing from closed history"
                << " closed=" << net.closedSeq(0)
                << " valid=" << net.validSeq(0)
                << " open=" << net.node(0).app().openLedger().current()->seq()
                << std::endl;
            return std::nullopt;
        }
        for (std::uint32_t n = 0; n <= observer; ++n)
            BEAST_EXPECT(net.validSeq(n) <= stallValid);

        std::uint32_t specSeq = 0;
        for (auto seq = stallValid + 1; seq <= net.closedSeq(0); ++seq)
        {
            if (ledgerHasTx(net.ledger(0, seq), origin))
            {
                specSeq = seq;
                break;
            }
        }
        if (!BEAST_EXPECT(specSeq > stallValid))
            return std::nullopt;
        BEAST_EXPECT(net.ledgerHash(0, specSeq) == net.ledgerHash(1, specSeq));
        BEAST_EXPECT(net.validSeq(0) < specSeq);
        BEAST_EXPECT(!ledgerHasTx(net.ledger(0, stallValid), origin));
        auto const specHash = net.ledgerHash(0, specSeq);
        auto const preHealClosed = net.closedSeq(0);
        auto const preHealClosedHash = net.ledgerHash(0, preHealClosed);
        auto const preHealReleases = originReleases(origin);
        BEAST_EXPECT(releasesAtSubmit == 0);
        BEAST_EXPECT(preHealReleases == 0);
        BEAST_EXPECT(stats->unauthorizedReleases[0] == 0);
        BEAST_EXPECT(stats->unauthorizedReleases[1] == 0);

        net.reconnectNode(2);
        auto const target = specSeq + ExportLimits::maxPublicationLedgers;
        net.runTo(target, SteppingNetwork::RunBudget{1600, 1'200'000});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
        {
            log << "  heal did not restore validation " << net.jobDiagnostics()
                << std::endl;
            return std::nullopt;
        }
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.validatedForkFree());
        BEAST_EXPECT(net.mode(2) == OperatingMode::FULL);
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);
        BEAST_EXPECT(!net.node(observer).app().getValidatorKeys().keys);
        BEAST_EXPECT(stats->secrets[observer] == 0);
        BEAST_EXPECT(stats->ownReleases[observer] == 0);
        for (std::uint32_t n = 0; n < observer; ++n)
            BEAST_EXPECT(stats->unauthorizedReleases[n] == 0);
        for (std::uint32_t n = 0; n <= observer; ++n)
            BEAST_EXPECT(net.ledgerHash(n, stallValid) == stallHash);

        bool originInValidated = false;
        for (auto seq = warmLedger; seq <= net.minValidatedSeq(); ++seq)
            originInValidated =
                originInValidated || ledgerHasTx(net.ledger(0, seq), origin);
        if (!BEAST_EXPECT(originInValidated))
            return std::nullopt;
        auto const witnessSeq = witnessAt(net, origin, warmLedger);
        if (!BEAST_EXPECT(witnessSeq != 0))
        {
            log << "  speculative origin did not witness after bounded heal"
                << " specSeq=" << specSeq
                << " validated=" << net.minValidatedSeq() << std::endl;
            return std::nullopt;
        }

        auto const finalTarget = net.minValidatedSeq();
        BEAST_EXPECT(net.ledgersAgree(finalTarget));
        BEAST_EXPECT(net.validatedForkFree());
        std::map<uint256, std::uint32_t> hits;
        std::set<uint256> unexpected;
        std::vector<uint256> outcome;
        for (auto seq = warmLedger; seq <= finalTarget; ++seq)
        {
            auto const canonical = net.ledger(0, seq);
            if (!BEAST_EXPECT(canonical != nullptr))
                return std::nullopt;
            for (std::uint32_t i = 1; i <= observer; ++i)
            {
                auto const ledger = net.ledger(i, seq);
                if (!BEAST_EXPECT(ledger != nullptr))
                    return std::nullopt;
                BEAST_EXPECT(ledger->info().hash == canonical->info().hash);
            }
            outcome.push_back(canonical->info().hash);
            for (auto const& [wtx, meta] : canonical->txs)
            {
                if (wtx->getTxnType() != ttEXPORT_SIGNATURES)
                    continue;
                if (!BEAST_EXPECT(meta != nullptr))
                    return std::nullopt;
                auto const id = wtx->getFieldH256(sfTransactionHash);
                if (id != origin)
                    unexpected.insert(id);
                else
                    ++hits[id];
                auto const txBytes = wtx->getSerializer().getData();
                auto const metaBytes = meta->getSerializer().getData();
                outcome.push_back(
                    sha512Half(makeSlice(txBytes), makeSlice(metaBytes)));
                for (std::uint32_t i = 0; i <= observer; ++i)
                {
                    auto const ledger = net.ledger(i, seq);
                    bool found = false;
                    for (auto const& [peerTx, peerMeta] : ledger->txs)
                    {
                        if (peerTx->getTxnType() != ttEXPORT_SIGNATURES ||
                            peerTx->getFieldH256(sfTransactionHash) != id)
                            continue;
                        found = true;
                        BEAST_EXPECT(
                            peerTx->getSerializer().getData() == txBytes);
                        BEAST_EXPECT(
                            peerMeta != nullptr &&
                            peerMeta->getSerializer().getData() == metaBytes);
                    }
                    BEAST_EXPECT(found);
                }
            }
        }
        BEAST_EXPECT(unexpected.empty());
        BEAST_EXPECT(hits[origin] == 1);
        log << "  no-quorum: quorum=" << quorum << " stallValid=" << stallValid
            << " specSeq=" << specSeq << " preHealClosed=" << preHealClosed
            << " releases=" << preHealReleases << " witness=" << witnessSeq
            << std::endl;
        outcome.push_back(stallHash);
        outcome.push_back(specHash);
        outcome.push_back(preHealClosedHash);
        outcome.push_back(origin);
        outcome.push_back(sha512Half(
            stallValid,
            specSeq,
            preHealClosed,
            static_cast<std::uint32_t>(preHealReleases),
            witnessSeq));
        return outcome;
    }

    std::optional<std::vector<uint256>>
    publicationWindowExpiry(SteppingNetwork& net)
    {
        using namespace std::chrono_literals;
        World world(net, true, true);
        if (!ready(world))
            return std::nullopt;

        test::StreamSink observerSink{beast::severities::kTrace};
        test::StreamSink validatorSink{beast::severities::kTrace};
        auto& observerCE = net.node(observer).app().getConsensusExtensions();
        auto& validatorCE = net.node(0).app().getConsensusExtensions();
        auto const previousObserver = observerCE.j_;
        auto const previousValidator = validatorCE.j_;
        observerCE.j_ = beast::Journal{observerSink};
        validatorCE.j_ = beast::Journal{validatorSink};
        scope_exit restoreJournals{[&]() {
            observerCE.j_ = previousObserver;
            validatorCE.j_ = previousValidator;
        }};

        auto const stats = world.observed;
        auto const funding = world.submit(
            observer,
            jtx::pay(jtx::Account::master, world.owner, jtx::XRP(10'000)),
            jtx::Account::master);
        if (!BEAST_EXPECT(funding && funding->getResult() == tesSUCCESS))
            return std::nullopt;
        net.runTo(warmLedger + 2);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger + 2))
            return std::nullopt;

        bool hold = true;
        bool countTimeouts = false;
        std::uint32_t heldDirect = 0;
        std::uint32_t heldProposals = 0;
        std::uint32_t gateTimeouts = 0;
        constexpr auto holdDelay = 180s;
        auto const holdShare = [&](std::uint16_t type, SimPipe::Frame bytes) {
            SimFault fault;
            if (!hold)
                return fault;
            if (type == protocol::mtEXPORT_SHARES)
            {
                ++heldDirect;
                fault.delay = holdDelay;
            }
            else if (type == protocol::mtPROPOSE_LEDGER)
            {
                auto const proposal =
                    decodeFrame<protocol::TMProposeSet>(bytes);
                if (proposal && proposal->exportsignatures_size() != 0)
                {
                    ++heldProposals;
                    fault.delay = holdDelay;
                }
            }
            return fault;
        };
        for (std::uint32_t from = 0; from <= observer; ++from)
            for (std::uint32_t to = 0; to <= observer; ++to)
                if (from != to)
                    net.faultFrames(from, to, holdShare);

        net.controller().observeJobs(
            [&](std::uint32_t id, JobType type, std::string const&) {
                if (!countTimeouts || id != 0 || type != jtACCEPT || !hold)
                    return;
                if (net.node(0)
                        .app()
                        .getConsensusExtensions()
                        .exportSigConvergenceFailed())
                    ++gateTimeouts;
            });

        auto const open = net.node(0).app().openLedger().current()->seq();
        auto const tx = world.submit(
            0,
            world.intent(
                world.owner, 1, open + ExportLimits::maxAdmissionWindowLedgers),
            world.owner);
        if (!BEAST_EXPECT(tx && tx->getResult() == tesSUCCESS))
            return std::nullopt;
        auto const origin = tx->getID();

        std::uint32_t admitSeq = 0;
        if (!BEAST_EXPECT(net.runUntil(
                [&] {
                    for (auto seq = warmLedger; seq <= net.validSeq(0); ++seq)
                        if (ledgerHasTx(net.ledger(0, seq), origin))
                        {
                            admitSeq = seq;
                            return true;
                        }
                    return false;
                },
                SteppingNetwork::RunBudget{40, 1'200'000})))
        {
            log << "  origin never validated under share hold"
                << " heldDirect=" << heldDirect
                << " heldProposals=" << heldProposals
                << " valid=" << net.validSeq(0)
                << " closed=" << net.closedSeq(0) << std::endl;
            return std::nullopt;
        }
        countTimeouts = true;

        auto const expirySeq = admitSeq + ExportLimits::maxPublicationLedgers;
        if (!BEAST_EXPECT(net.runUntil(
                [&] {
                    return net.minValidatedSeq() > expirySeq &&
                        gateTimeouts > 0;
                },
                SteppingNetwork::RunBudget{120, 1'200'000})))
        {
            log << "  publication window did not expire while frames were held"
                << " admit=" << admitSeq << " expiry=" << expirySeq
                << " valid=" << net.minValidatedSeq()
                << " gateTimeouts=" << gateTimeouts
                << " heldDirect=" << heldDirect
                << " heldProposals=" << heldProposals << std::endl;
            return std::nullopt;
        }
        std::uint32_t timeoutLines = 0;
        {
            std::istringstream in{validatorSink.messages().str()};
            for (std::string line; std::getline(in, line);)
                if (line.find("Export: signature-set publication timeout") !=
                        std::string::npos ||
                    line.find(
                        "Export: exportSigSet quorum alignment timeout") !=
                        std::string::npos)
                    ++timeoutLines;
        }
        BEAST_EXPECT(heldDirect + heldProposals > 0);
        BEAST_EXPECT(gateTimeouts > 0);
        BEAST_EXPECT(timeoutLines > 0);
        if (!BEAST_EXPECT(stats->directFrames[observer] == 0))
        {
            log << "  direct Export frames reached the observer during hold"
                << " direct=" << stats->directFrames[observer]
                << " heldDirect=" << heldDirect
                << " heldProposals=" << heldProposals << std::endl;
            return std::nullopt;
        }
        if (!BEAST_EXPECT(witnessAt(net, origin, warmLedger) == 0))
        {
            log << "  origin witnessed while frames were held"
                << " witness=" << witnessAt(net, origin, warmLedger)
                << std::endl;
            return std::nullopt;
        }
        for (std::uint32_t n = 0; n < observer; ++n)
            BEAST_EXPECT(stats->unauthorizedReleases[n] == 0);

        auto const beforeRelease = observerSink.messages().str().size();
        hold = false;
        for (std::uint32_t from = 0; from <= observer; ++from)
            for (std::uint32_t to = 0; to <= observer; ++to)
                if (from != to)
                    net.faultFrames(from, to, {});
        net.controller().observeJobs({});

        auto const originText = "origin=" + to_string(origin);
        auto sawExpired = [&] {
            std::istringstream in{
                observerSink.messages().str().substr(beforeRelease)};
            for (std::string line; std::getline(in, line);)
            {
                if (line.find("ExportShare: resolution") == std::string::npos)
                    continue;
                if (line.find(originText) == std::string::npos)
                    continue;
                if (line.find("reason=publication-expired") !=
                    std::string::npos)
                    return true;
            }
            return false;
        };
        if (!BEAST_EXPECT(net.runUntil(
                [&] {
                    return sawExpired() && stats->directFrames[observer] > 0;
                },
                SteppingNetwork::RunBudget{holdDelay.count() + 20, 1'200'000})))
        {
            log << "  held frames were not rejected as publication-expired"
                << " direct=" << stats->directFrames[observer] << std::endl;
            std::istringstream in{
                observerSink.messages().str().substr(beforeRelease)};
            for (std::string line; std::getline(in, line);)
                if (line.find(originText) != std::string::npos &&
                    line.find("ExportShare:") != std::string::npos)
                    log << "    " << line << std::endl;
            return std::nullopt;
        }

        auto const expiredAt = net.minValidatedSeq();
        for (auto seq = warmLedger; seq <= expiredAt; ++seq)
        {
            for (std::uint32_t n = 0; n <= observer; ++n)
            {
                auto const ledger = net.ledger(n, seq);
                if (!BEAST_EXPECT(ledger != nullptr))
                    return std::nullopt;
                BEAST_EXPECT(ledger->info().hash == net.ledgerHash(0, seq));
                for (auto const& [wtx, meta] : ledger->txs)
                {
                    if (wtx->getTxnType() != ttEXPORT_SIGNATURES)
                        continue;
                    BEAST_EXPECT(
                        wtx->getFieldH256(sfTransactionHash) != origin);
                }
            }
        }

        auto const ownerId = world.owner.id();
        auto const latchKey = keylet::exportLatch(ownerId, origin);
        auto const releaseLedger = net.ledger(0, expiredAt);
        if (!BEAST_EXPECT(releaseLedger != nullptr))
            return std::nullopt;
        auto const ownerAtRelease =
            releaseLedger->read(keylet::account(ownerId))
                ->getFieldU32(sfOwnerCount);
        for (std::uint32_t n = 0; n <= observer; ++n)
        {
            auto const ledger = net.ledger(n, expiredAt);
            auto const latch = ledger->read(latchKey);
            if (!BEAST_EXPECT(
                    latch && latch->getType() == ltEXPORT_LATCH &&
                    latch->isFieldPresent(sfExportNode) &&
                    !latch->isFieldPresent(sfExportSignatureHash) &&
                    latch->getFieldU32(sfLastLedgerSequence) == expirySeq &&
                    pendingDirContains(*ledger, latchKey.key)))
                return std::nullopt;
        }

        auto const dupOpen = net.node(0).app().openLedger().current()->seq();
        auto const reused = world.submit(
            0,
            world.intent(
                world.owner,
                1,
                dupOpen + ExportLimits::maxAdmissionWindowLedgers),
            world.owner);
        if (!BEAST_EXPECT(reused != nullptr))
            return std::nullopt;
        auto const reusedId = reused->getID();
        if (!BEAST_EXPECT(net.runUntil(
                [&] {
                    auto const result = validatedResult(net, reusedId);
                    return result && *result == tecDUPLICATE;
                },
                SteppingNetwork::RunBudget{40, 1'200'000})))
        {
            log << "  reused ticket was not tecDUPLICATE in a validated ledger"
                << " submit=" << transHuman(reused->getResult()) << std::endl;
            return std::nullopt;
        }
        std::uint32_t dupSeq = 0;
        for (auto seq = warmLedger; seq <= net.minValidatedSeq(); ++seq)
            if (ledgerHasTx(net.ledger(0, seq), reusedId))
                dupSeq = seq;
        auto const prunedLedger = net.ledger(0, dupSeq);
        auto const pruned =
            prunedLedger ? prunedLedger->read(latchKey) : nullptr;
        auto const ownerNow = prunedLedger
            ? prunedLedger->read(keylet::account(ownerId))
                  ->getFieldU32(sfOwnerCount)
            : 0;
        // tecDUPLICATE is claim-hard: Transactor resets the view, so the
        // prune inside that failed intent does not stick. The reserve stays
        // and the pending link is still there.
        if (!BEAST_EXPECT(
                pruned && pruned->isFieldPresent(sfExportNode) &&
                pendingDirContains(*prunedLedger, latchKey.key) &&
                ownerNow == ownerAtRelease))
            return std::nullopt;

        auto const freshOpen = net.node(0).app().openLedger().current()->seq();
        auto const fresh = world.submit(
            0,
            world.intent(
                world.owner,
                2,
                freshOpen + ExportLimits::maxAdmissionWindowLedgers),
            world.owner);
        if (!BEAST_EXPECT(fresh && fresh->getResult() == tesSUCCESS))
            return std::nullopt;
        auto const freshOrigin = fresh->getID();
        auto const finalTarget = net.minValidatedSeq() + 6;
        net.runTo(finalTarget, SteppingNetwork::RunBudget{1600, 1'200'000});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= finalTarget))
            return std::nullopt;

        auto const freshWitness = witnessAt(net, freshOrigin, warmLedger);
        if (!BEAST_EXPECT(freshWitness != 0))
            return std::nullopt;
        BEAST_EXPECT(witnessAt(net, origin, warmLedger) == 0);
        for (std::uint32_t n = 0; n < observer; ++n)
            BEAST_EXPECT(stats->unauthorizedReleases[n] == 0);
        auto const afterFresh = net.ledger(0, net.minValidatedSeq());
        auto const retained = afterFresh->read(latchKey);
        BEAST_EXPECT(
            retained && !retained->isFieldPresent(sfExportNode) &&
            !pendingDirContains(*afterFresh, latchKey.key));
        BEAST_EXPECT(
            afterFresh->read(keylet::account(ownerId))
                ->getFieldU32(sfOwnerCount) == ownerAtRelease + 1);

        std::map<uint256, std::uint32_t> hits;
        std::vector<uint256> outcome;
        for (auto seq = warmLedger; seq <= net.minValidatedSeq(); ++seq)
        {
            auto const canonical = net.ledger(0, seq);
            if (!BEAST_EXPECT(canonical != nullptr))
                return std::nullopt;
            for (std::uint32_t i = 1; i <= observer; ++i)
            {
                auto const ledger = net.ledger(i, seq);
                if (!BEAST_EXPECT(ledger != nullptr))
                    return std::nullopt;
                BEAST_EXPECT(ledger->info().hash == canonical->info().hash);
            }
            outcome.push_back(canonical->info().hash);
            for (auto const& [wtx, meta] : canonical->txs)
            {
                if (wtx->getTxnType() != ttEXPORT_SIGNATURES)
                    continue;
                if (!BEAST_EXPECT(meta != nullptr))
                    return std::nullopt;
                auto const id = wtx->getFieldH256(sfTransactionHash);
                ++hits[id];
                BEAST_EXPECT(id != origin);
                auto const txBytes = wtx->getSerializer().getData();
                auto const metaBytes = meta->getSerializer().getData();
                outcome.push_back(
                    sha512Half(makeSlice(txBytes), makeSlice(metaBytes)));
                for (std::uint32_t i = 0; i <= observer; ++i)
                {
                    auto const ledger = net.ledger(i, seq);
                    bool found = false;
                    for (auto const& [peerTx, peerMeta] : ledger->txs)
                    {
                        if (peerTx->getTxnType() != ttEXPORT_SIGNATURES ||
                            peerTx->getFieldH256(sfTransactionHash) != id)
                            continue;
                        found = true;
                        BEAST_EXPECT(
                            peerTx->getSerializer().getData() == txBytes);
                        BEAST_EXPECT(
                            peerMeta != nullptr &&
                            peerMeta->getSerializer().getData() == metaBytes);
                    }
                    BEAST_EXPECT(found);
                }
            }
        }
        BEAST_EXPECT(hits[freshOrigin] == 1);
        BEAST_EXPECT(hits[origin] == 0);
        log << "  publication-expiry: admit=" << admitSeq
            << " expiry=" << expirySeq << " validAtRelease=" << expiredAt
            << " heldDirect=" << heldDirect
            << " heldProposals=" << heldProposals
            << " gateTimeouts=" << gateTimeouts
            << " freshWitness=" << freshWitness << std::endl;
        outcome.push_back(origin);
        outcome.push_back(freshOrigin);
        outcome.push_back(sha512Half(
            admitSeq,
            expirySeq,
            heldDirect,
            heldProposals,
            gateTimeouts,
            freshWitness));
        return outcome;
    }

    std::optional<std::vector<uint256>>
    candidateChangeInsideDeadline(SteppingNetwork& net)
    {
        using namespace std::chrono_literals;
        World world(net, true, true);
        if (!ready(world))
            return std::nullopt;

        test::StreamSink sink{beast::severities::kTrace};
        auto& validatorCE = net.node(0).app().getConsensusExtensions();
        auto const previousJournal = validatorCE.j_;
        validatorCE.j_ = beast::Journal{sink};
        scope_exit restoreJournal{[&]() { validatorCE.j_ = previousJournal; }};

        auto const stats = world.observed;
        auto const funding = world.submit(
            observer,
            jtx::pay(jtx::Account::master, world.owner, jtx::XRP(10'000)),
            jtx::Account::master);
        if (!BEAST_EXPECT(funding && funding->getResult() == tesSUCCESS))
            return std::nullopt;
        net.runTo(warmLedger + 2);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger + 2))
            return std::nullopt;

        bool hold = true;
        std::uint32_t held = 0;
        bool waitedPastDeadline = false;
        auto const roundDeadline =
            ripple::detail::sidecarConvergenceTimeout(ConsensusParms{});
        auto const releaseAfter = roundDeadline - 1s;
        auto const holdMinority = [&](std::uint16_t type,
                                      SimPipe::Frame bytes) {
            SimFault fault;
            if (!hold)
                return fault;
            if (type == protocol::mtEXPORT_SHARES)
            {
                ++held;
                fault.drop = true;
            }
            else if (type == protocol::mtPROPOSE_LEDGER)
            {
                auto const proposal =
                    decodeFrame<protocol::TMProposeSet>(bytes);
                if (proposal && proposal->exportsignatures_size() != 0)
                {
                    ++held;
                    fault.drop = true;
                }
            }
            return fault;
        };
        for (std::uint32_t to = 0; to <= observer; ++to)
            if (to != 2)
                net.faultFrames(2, to, holdMinority);

        net.controller().observeJobs(
            [&](std::uint32_t id, JobType, std::string const&) {
                if (id != 0)
                    return;
                auto const& ce = net.node(0).app().getConsensusExtensions();
                if (!ce.exportSigGateStarted_ ||
                    ce.exportSigConvergenceFailed())
                    return;
                auto const elapsed =
                    net.controller().now() - ce.exportSigGateStart_;
                if (elapsed > roundDeadline)
                    waitedPastDeadline = true;
            });

        auto const open = net.node(0).app().openLedger().current()->seq();
        auto const tx = world.submit(
            0,
            world.intent(
                world.owner, 1, open + ExportLimits::maxAdmissionWindowLedgers),
            world.owner);
        if (!BEAST_EXPECT(tx && tx->getResult() == tesSUCCESS))
            return std::nullopt;
        auto const origin = tx->getID();

        if (!BEAST_EXPECT(net.runUntil(
                [&] {
                    bool admitted = false;
                    for (auto seq = warmLedger; seq <= net.validSeq(0); ++seq)
                        admitted =
                            admitted || ledgerHasTx(net.ledger(0, seq), origin);
                    if (!admitted || held == 0)
                        return false;
                    auto const& ce = net.node(0).app().getConsensusExtensions();
                    if (!ce.exportSigGateStarted_ ||
                        ce.exportSigConvergenceFailed())
                        return false;
                    auto const elapsed =
                        net.controller().now() - ce.exportSigGateStart_;
                    return elapsed >= releaseAfter && elapsed <= roundDeadline;
                },
                SteppingNetwork::RunBudget{80, 1'200'000})))
        {
            auto const& ce = net.node(0).app().getConsensusExtensions();
            log << "  minority hold missed the open gate window"
                << " held=" << held << " gate=" << ce.exportSigGateStarted_
                << " failed=" << ce.exportSigConvergenceFailed()
                << " valid=" << net.validSeq(0) << std::endl;
            return std::nullopt;
        }

        auto const elapsedAtRelease =
            net.controller().now() - validatorCE.exportSigGateStart_;
        auto const gateSeq = net.closedSeq(0) + 1;
        hold = false;
        for (std::uint32_t to = 0; to <= observer; ++to)
            if (to != 2)
                net.faultFrames(2, to, {});
        auto const journalMark = sink.messages().str().size();
        net.controller().observeJobs({});

        auto const target = net.minValidatedSeq() + 8;
        net.runTo(target, SteppingNetwork::RunBudget{1600, 1'200'000});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
            return std::nullopt;
        BEAST_EXPECT(!waitedPastDeadline);
        auto const seqW = witnessAt(net, origin, warmLedger);
        if (!BEAST_EXPECT(seqW != 0))
            return std::nullopt;
        BEAST_EXPECT(stats->builds[observer].contains(
            {seqW, net.ledgerHash(0, seqW - 1), net.ledgerHash(0, seqW)}));

        std::uint32_t publishedAfter = 0;
        {
            std::istringstream in{sink.messages().str().substr(journalMark)};
            for (std::string line; std::getline(in, line);)
                if (line.find("Export: published exportSigSetHash") !=
                        std::string::npos ||
                    line.find("Export: refreshed exportSigSetHash") !=
                        std::string::npos)
                    ++publishedAfter;
        }
        if (!BEAST_EXPECT(publishedAfter > 0))
        {
            log << "  no exportSigSetHash publish/refresh after the gate"
                << std::endl;
            return std::nullopt;
        }

        BEAST_EXPECT(net.ledgersAgree(net.minValidatedSeq()));
        BEAST_EXPECT(net.validatedForkFree());
        BEAST_EXPECT(!net.node(observer).app().getValidatorKeys().keys);
        BEAST_EXPECT(stats->secrets[observer] == 0);
        for (std::uint32_t n = 0; n < observer; ++n)
            BEAST_EXPECT(stats->unauthorizedReleases[n] == 0);

        std::map<uint256, std::uint32_t> hits;
        std::vector<uint256> outcome;
        for (auto seq = warmLedger; seq <= net.minValidatedSeq(); ++seq)
        {
            auto const canonical = net.ledger(0, seq);
            if (!BEAST_EXPECT(canonical != nullptr))
                return std::nullopt;
            for (std::uint32_t i = 1; i <= observer; ++i)
            {
                auto const ledger = net.ledger(i, seq);
                if (!BEAST_EXPECT(ledger != nullptr))
                    return std::nullopt;
                BEAST_EXPECT(ledger->info().hash == canonical->info().hash);
            }
            outcome.push_back(canonical->info().hash);
            for (auto const& [wtx, meta] : canonical->txs)
            {
                if (wtx->getTxnType() != ttEXPORT_SIGNATURES)
                    continue;
                if (!BEAST_EXPECT(meta != nullptr))
                    return std::nullopt;
                auto const id = wtx->getFieldH256(sfTransactionHash);
                ++hits[id];
                auto const txBytes = wtx->getSerializer().getData();
                auto const metaBytes = meta->getSerializer().getData();
                outcome.push_back(
                    sha512Half(makeSlice(txBytes), makeSlice(metaBytes)));
                for (std::uint32_t i = 0; i <= observer; ++i)
                {
                    auto const ledger = net.ledger(i, seq);
                    bool found = false;
                    for (auto const& [peerTx, peerMeta] : ledger->txs)
                    {
                        if (peerTx->getTxnType() != ttEXPORT_SIGNATURES ||
                            peerTx->getFieldH256(sfTransactionHash) != id)
                            continue;
                        found = true;
                        BEAST_EXPECT(
                            peerTx->getSerializer().getData() == txBytes);
                        BEAST_EXPECT(
                            peerMeta != nullptr &&
                            peerMeta->getSerializer().getData() == metaBytes);
                    }
                    BEAST_EXPECT(found);
                }
            }
        }
        BEAST_EXPECT(hits[origin] == 1);
        log << "  candidate-change: held=" << held << " releaseElapsedMs="
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   elapsedAtRelease)
                   .count()
            << " gateSeq=" << gateSeq << " publishedAfter=" << publishedAfter
            << " witness=" << seqW << std::endl;
        outcome.push_back(origin);
        outcome.push_back(sha512Half(
            held,
            static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    elapsedAtRelease)
                    .count()),
            gateSeq,
            publishedAfter,
            seqW));
        return outcome;
    }

    std::optional<std::vector<uint256>>
    lateMaterialAfterDeadline(SteppingNetwork& net)
    {
        using namespace std::chrono_literals;
        World world(net, true, true);
        if (!ready(world))
            return std::nullopt;

        test::StreamSink observerSink{beast::severities::kTrace};
        test::StreamSink validatorSink{beast::severities::kTrace};
        auto& observerCE = net.node(observer).app().getConsensusExtensions();
        auto& validatorCE = net.node(0).app().getConsensusExtensions();
        auto const previousObserver = observerCE.j_;
        auto const previousValidator = validatorCE.j_;
        observerCE.j_ = beast::Journal{observerSink};
        validatorCE.j_ = beast::Journal{validatorSink};
        scope_exit restoreJournal{[&]() {
            observerCE.j_ = previousObserver;
            validatorCE.j_ = previousValidator;
        }};

        auto const stats = world.observed;
        auto const funding = world.submit(
            observer,
            jtx::pay(jtx::Account::master, world.owner, jtx::XRP(10'000)),
            jtx::Account::master);
        if (!BEAST_EXPECT(funding && funding->getResult() == tesSUCCESS))
            return std::nullopt;
        net.runTo(warmLedger + 2);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger + 2))
            return std::nullopt;

        bool hold = true;
        bool sawTimeoutAccept = false;
        std::uint32_t timeoutSeq = 0;
        std::uint32_t held = 0;
        std::array<std::uint32_t, 4> postTotal{};
        std::array<std::uint32_t, 4> postVal{};
        std::array<std::uint32_t, 4> postProp{};
        std::array<std::uint32_t, 4> postDirect{};
        std::uint32_t postTotal0to2 = 0;
        std::uint32_t postVal0to2 = 0;
        std::uint32_t postProp0to2 = 0;
        std::uint32_t postDirect0to2 = 0;
        auto countWire = [](std::uint16_t type,
                            std::uint32_t& total,
                            std::uint32_t& validations,
                            std::uint32_t& proposals,
                            std::uint32_t& directs) {
            ++total;
            if (type == protocol::mtVALIDATION)
                ++validations;
            else if (type == protocol::mtPROPOSE_LEDGER)
                ++proposals;
            else if (type == protocol::mtEXPORT_SHARES)
                ++directs;
        };
        for (std::uint32_t to = 0; to <= observer; ++to)
        {
            if (to == 2)
                continue;
            net.faultFrames(
                2,
                to,
                [&, to](std::uint16_t type, SimPipe::Frame bytes) {
                    SimFault fault;
                    if (!hold)
                    {
                        countWire(
                            type,
                            postTotal[to],
                            postVal[to],
                            postProp[to],
                            postDirect[to]);
                        (void)bytes;
                        return fault;
                    }
                    if (type == protocol::mtEXPORT_SHARES)
                    {
                        ++held;
                        fault.drop = true;
                    }
                    else if (type == protocol::mtPROPOSE_LEDGER)
                    {
                        auto const proposal =
                            decodeFrame<protocol::TMProposeSet>(bytes);
                        if (proposal && proposal->exportsignatures_size() != 0)
                        {
                            ++held;
                            fault.drop = true;
                        }
                    }
                    return fault;
                });
        }
        net.faultFrames(
            0,
            2,
            [&](std::uint16_t type, SimPipe::Frame) {
                SimFault fault;
                if (!hold)
                    countWire(
                        type,
                        postTotal0to2,
                        postVal0to2,
                        postProp0to2,
                        postDirect0to2);
                return fault;
            });
        test::StreamSink senderSink{beast::severities::kTrace};
        auto& senderCE = net.node(2).app().getConsensusExtensions();
        auto const previousSender = senderCE.j_;
        senderCE.j_ = beast::Journal{senderSink};
        scope_exit restoreSender{[&]() { senderCE.j_ = previousSender; }};
        std::size_t releaseMark = 0;
        int modeMin = 1000;
        int modeMax = -1;
        bool sawLive = false;
        bool sawDead = false;
        std::set<std::uint32_t> sampledRounds;
        std::string rounds;
        auto summarize = [](Json::Value const& info) {
            std::ostringstream out;
            out << "proposing="
                << (info.isMember("proposing") && info["proposing"].asBool())
                << " validating="
                << (info.isMember("validating") && info["validating"].asBool())
                << " proposers="
                << (info.isMember("proposers") ? info["proposers"].asInt()
                                               : -1)
                << " peer_positions="
                << (info.isMember("peer_positions")
                        ? static_cast<int>(info["peer_positions"].size())
                        : 0);
            return out.str();
        };
        net.controller().observeJobs(
            [&](std::uint32_t id, JobType type, std::string const&) {
                if (id != 0 || type != jtACCEPT)
                    return;
                if (!sawTimeoutAccept)
                {
                    if (!hold)
                        return;
                    auto const& ce =
                        net.node(0).app().getConsensusExtensions();
                    if (!ce.exportSigConvergenceFailed())
                        return;
                    sawTimeoutAccept = true;
                    timeoutSeq = net.closedSeq(0) + 1;
                    // Lift before the rest of this beat so 518+ is measured
                    // while validator 2 is allowed to send.
                    hold = false;
                    releaseMark = senderSink.messages().str().size();
                    return;
                }
                auto const building = net.closedSeq(0) + 1;
                if (building <= timeoutSeq ||
                    building > timeoutSeq + ExportLimits::maxPublicationLedgers ||
                    !sampledRounds.insert(building).second)
                    return;
                auto const mode = static_cast<int>(net.mode(2));
                modeMin = std::min(modeMin, mode);
                modeMax = std::max(modeMax, mode);
                if (net.isLive(2))
                    sawLive = true;
                else
                    sawDead = true;
                rounds += " seq=" + std::to_string(building) + " n2{" +
                    summarize(
                        net.node(2).app().getOPs().getConsensusInfo()) +
                    "} n0{" +
                    summarize(
                        net.node(0).app().getOPs().getConsensusInfo()) +
                    "}";
            });

        auto const open = net.node(0).app().openLedger().current()->seq();
        auto const tx = world.submit(
            0,
            world.intent(
                world.owner, 1, open + ExportLimits::maxAdmissionWindowLedgers),
            world.owner);
        if (!BEAST_EXPECT(tx && tx->getResult() == tesSUCCESS))
            return std::nullopt;
        auto const origin = tx->getID();

        if (!BEAST_EXPECT(net.runUntil(
                [&] {
                    return sawTimeoutAccept && timeoutSeq != 0 &&
                        net.validSeq(0) >= timeoutSeq;
                },
                SteppingNetwork::RunBudget{80, 1'200'000})))
        {
            log << "  gate did not time out before the minority release"
                << " held=" << held << " timeoutAccept=" << sawTimeoutAccept
                << " timeoutSeq=" << timeoutSeq
                << " valid=" << net.validSeq(0) << std::endl;
            return std::nullopt;
        }

        std::uint32_t admitSeq = 0;
        for (auto seq = warmLedger; seq <= net.validSeq(0); ++seq)
            if (ledgerHasTx(net.ledger(0, seq), origin))
                admitSeq = seq;
        if (!BEAST_EXPECT(admitSeq != 0))
            return std::nullopt;
        auto const decidedSeq = timeoutSeq;
        log << "  post-release wires: val/prop/direct/total"
            << " 2to0=" << postVal[0] << "/" << postProp[0] << "/"
            << postDirect[0] << "/" << postTotal[0]
            << " 2to1=" << postVal[1] << "/" << postProp[1] << "/"
            << postDirect[1] << "/" << postTotal[1]
            << " 2toObs=" << postVal[observer] << "/" << postProp[observer]
            << "/" << postDirect[observer] << "/" << postTotal[observer]
            << " 0to2=" << postVal0to2 << "/" << postProp0to2 << "/"
            << postDirect0to2 << "/" << postTotal0to2
            << " mode2=" << modeMin << ".." << modeMax
            << " live=" << sawLive << "/" << sawDead
            << " validNow=" << net.validSeq(0) << rounds << std::endl;
        if (!BEAST_EXPECT(
                decidedSeq <= admitSeq + ExportLimits::maxPublicationLedgers))
        {
            log << "  timed-out round is already outside the publication window"
                << " admit=" << admitSeq << " timeoutSeq=" << timeoutSeq
                << " decided=" << decidedSeq << " held=" << held << std::endl;
            return std::nullopt;
        }
        BEAST_EXPECT(net.ledgersAgree(decidedSeq));
        BEAST_EXPECT(witnessAt(net, origin, warmLedger) == 0);
        for (std::uint32_t n = 0; n <= observer; ++n)
        {
            auto const ledger = net.ledger(n, decidedSeq);
            if (!BEAST_EXPECT(ledger != nullptr))
                return std::nullopt;
            for (auto const& [wtx, meta] : ledger->txs)
                if (wtx->getTxnType() == ttEXPORT_SIGNATURES)
                    BEAST_EXPECT(
                        wtx->getFieldH256(sfTransactionHash) != origin);
        }

        auto const observerMark = observerSink.messages().str().size();
        auto const validatorMark = validatorSink.messages().str().size();
        hold = false;
        auto const windowEnd = admitSeq + ExportLimits::maxPublicationLedgers;
        if (!BEAST_EXPECT(net.runUntil(
                [&] {
                    auto const mode = static_cast<int>(net.mode(2));
                    modeMin = std::min(modeMin, mode);
                    modeMax = std::max(modeMax, mode);
                    if (net.isLive(2))
                        sawLive = true;
                    else
                        sawDead = true;
                    auto const seq = net.validSeq(0);
                    if (seq > decidedSeq && seq <= windowEnd &&
                        sampledRounds.insert(seq).second)
                    {
                        rounds += " seq=" + std::to_string(seq) + " n2{" +
                            summarize(net.node(2)
                                          .app()
                                          .getOPs()
                                          .getConsensusInfo()) +
                            "} n0{" +
                            summarize(net.node(0)
                                          .app()
                                          .getOPs()
                                          .getConsensusInfo()) +
                            "}";
                    }
                    return net.minValidatedSeq() >= windowEnd;
                },
                SteppingNetwork::RunBudget{40, 1'200'000})))
            return std::nullopt;

        auto const accepted = std::to_string(
            static_cast<unsigned>(ExportSigCollector::AdmitResult::accepted));
        auto const originText = "origin=" + to_string(origin);
        bool sawAccepted = false;
        {
            std::istringstream in{
                observerSink.messages().str().substr(observerMark)};
            for (std::string line; std::getline(in, line);)
            {
                if (line.find("ExportShare: collector commit") ==
                    std::string::npos)
                    continue;
                if (line.find(originText) == std::string::npos)
                    continue;
                if (line.find("signatureVerified=true") == std::string::npos)
                    continue;
                if (line.find("result=" + accepted) == std::string::npos)
                    continue;
                sawAccepted = true;
                break;
            }
        }
        auto const seqW = witnessAt(net, origin, warmLedger);
        if (!BEAST_EXPECT(
                sawAccepted && seqW != 0 && seqW > decidedSeq &&
                seqW <= windowEnd))
        {
            std::uint32_t localRelease = 0;
            {
                std::istringstream in{senderSink.messages().str().substr(releaseMark)};
                for (std::string line; std::getline(in, line);)
                    if (line.find("ExportShare: local release frame") !=
                            std::string::npos &&
                        line.find(originText) != std::string::npos)
                        ++localRelease;
            }
            log << "  late material after the deadline did not witness"
                << " see .ai-docs/reviews/2026-09-22-dsf-b2c-red-claude-review.md"
                << " decided=" << decidedSeq << " admit=" << admitSeq
                << " windowEnd=" << windowEnd << " witness=" << seqW
                << " accepted=" << sawAccepted
                << " postDirect0=" << postDirect[0]
                << " postDirect1=" << postDirect[1]
                << " postDirectObs=" << postDirect[observer]
                << " postProp0=" << postProp[0] << " postProp1=" << postProp[1]
                << " postPropObs=" << postProp[observer]
                << " postVal0=" << postVal[0] << " postVal1=" << postVal[1]
                << " postValObs=" << postVal[observer]
                << " postTotal0=" << postTotal[0]
                << " postTotal1=" << postTotal[1]
                << " postTotalObs=" << postTotal[observer]
                << " from0 val=" << postVal0to2 << " prop=" << postProp0to2
                << " direct=" << postDirect0to2 << " total=" << postTotal0to2
                << " localRelease=" << localRelease
                << " mode2=" << modeMin << ".." << modeMax
                << " live2=" << sawLive << "/" << sawDead << rounds
                << std::endl;
            auto const [quorum, trusted] =
                net.node(0).app().validators().getQuorumKeys();
            log << "  quorum=" << quorum << " trusted=" << trusted.size()
                << std::endl;
            for (auto const& link : net.linkHistory())
            {
                if (!link.touches(2))
                    continue;
                log << "  link " << link.a << "-" << link.b
                    << " severed=" << (link.wire && link.wire->severed())
                    << std::endl;
            }
            auto dump = [&](char const* who, std::string const& text) {
                std::istringstream in{text};
                for (std::string line; std::getline(in, line);)
                    if (line.find(originText) != std::string::npos &&
                        line.find("ExportShare:") != std::string::npos)
                        log << "    " << who << " " << line << std::endl;
            };
            dump("observer", observerSink.messages().str().substr(observerMark));
            dump(
                "validator",
                validatorSink.messages().str().substr(validatorMark));
            return std::nullopt;
        }

        BEAST_EXPECT(net.ledgersAgree(net.minValidatedSeq()));
        BEAST_EXPECT(net.validatedForkFree());
        BEAST_EXPECT(!net.node(observer).app().getValidatorKeys().keys);
        for (std::uint32_t n = 0; n < observer; ++n)
            BEAST_EXPECT(stats->unauthorizedReleases[n] == 0);
        BEAST_EXPECT(stats->builds[observer].contains(
            {seqW, net.ledgerHash(0, seqW - 1), net.ledgerHash(0, seqW)}));

        std::map<uint256, std::uint32_t> hits;
        std::vector<uint256> outcome;
        for (auto seq = warmLedger; seq <= net.minValidatedSeq(); ++seq)
        {
            auto const canonical = net.ledger(0, seq);
            if (!BEAST_EXPECT(canonical != nullptr))
                return std::nullopt;
            for (std::uint32_t i = 1; i <= observer; ++i)
            {
                auto const ledger = net.ledger(i, seq);
                if (!BEAST_EXPECT(ledger != nullptr))
                    return std::nullopt;
                BEAST_EXPECT(ledger->info().hash == canonical->info().hash);
            }
            outcome.push_back(canonical->info().hash);
            for (auto const& [wtx, meta] : canonical->txs)
            {
                if (wtx->getTxnType() != ttEXPORT_SIGNATURES)
                    continue;
                if (!BEAST_EXPECT(meta != nullptr))
                    return std::nullopt;
                auto const id = wtx->getFieldH256(sfTransactionHash);
                ++hits[id];
                auto const txBytes = wtx->getSerializer().getData();
                auto const metaBytes = meta->getSerializer().getData();
                outcome.push_back(
                    sha512Half(makeSlice(txBytes), makeSlice(metaBytes)));
                for (std::uint32_t i = 0; i <= observer; ++i)
                {
                    auto const ledger = net.ledger(i, seq);
                    bool found = false;
                    for (auto const& [peerTx, peerMeta] : ledger->txs)
                    {
                        if (peerTx->getTxnType() != ttEXPORT_SIGNATURES ||
                            peerTx->getFieldH256(sfTransactionHash) != id)
                            continue;
                        found = true;
                        BEAST_EXPECT(
                            peerTx->getSerializer().getData() == txBytes);
                        BEAST_EXPECT(
                            peerMeta != nullptr &&
                            peerMeta->getSerializer().getData() == metaBytes);
                    }
                    BEAST_EXPECT(found);
                }
            }
        }
        BEAST_EXPECT(hits[origin] == 1);
        log << "  late-after-deadline: held=" << held << " admit=" << admitSeq
            << " decided=" << decidedSeq << " windowEnd=" << windowEnd
            << " witness=" << seqW << " accepted=" << sawAccepted << std::endl;
        outcome.push_back(origin);
        outcome.push_back(sha512Half(held, admitSeq, decidedSeq, seqW));
        return outcome;
    }

public:
    void
    run() override
    {
        // Optional focused iteration, e.g. --unittest-arg=case=validator.
        // Keep replays=N available to the existing replay combinator.
        std::string filter;
        if (auto const at = arg().find("case="); at != std::string::npos)
        {
            filter = arg().substr(at + 5);
            filter = filter.substr(0, filter.find(','));
        }
        std::size_t selected = 0;
        auto matches = [&](std::string const& label) {
            if (!filter.empty() && label.find(filter) == std::string::npos)
                return false;
            ++selected;
            return true;
        };
        for (auto const rng : {false, true})
            for (auto const exportOn : {false, true})
            {
                auto const label = std::string{"feature matrix rng="} +
                    (rng ? "on" : "off") +
                    " export=" + (exportOn ? "on" : "off");
                if (!matches(label))
                    continue;
                testcase(label);
                expectReplays(
                    *this,
                    label.c_str(),
                    [this, rng, exportOn](SteppingNetwork& net) {
                        return scenario(net, rng, exportOn);
                    });
            }
        for (auto const& [label, fault, rng, exportOn] :
             {std::tuple{
                  "observer gets Export material only through proposals",
                  Fault::noDirectShares,
                  false,
                  true},
              std::tuple{
                  "observer gets Export material only through direct messages",
                  Fault::noProposalShares,
                  false,
                  true},
              std::tuple{
                  "observer misses both Export routes and acquires the witness "
                  "ledger",
                  Fault::noShares,
                  false,
                  true},
              std::tuple{
                  "duplicated proposals validations and Export material do not "
                  "multiply effects",
                  Fault::duplicateTraffic,
                  true,
                  true},
              std::tuple{
                  "observer builds five seconds late while validations advance",
                  Fault::slowObserver,
                  true,
                  true},
              std::tuple{
                  "observer hears validations five seconds late",
                  Fault::lateValidations,
                  true,
                  true},
              std::tuple{
                  "observer hears validations twenty seconds late",
                  Fault::veryLateValidations,
                  true,
                  true},
              std::tuple{
                  "one RNG validator with slow builds and traffic recovers",
                  Fault::slowValidator,
                  true,
                  false},
              std::tuple{
                  "sluggish RNG validator recovers with Export enabled but "
                  "idle",
                  Fault::slowValidatorIdleExport,
                  true,
                  true},
              std::tuple{
                  "independent gates: Export accepted while RNG reveals are "
                  "delayed",
                  Fault::delayedRngReveals,
                  true,
                  true},
              std::tuple{
                  "independent gates: RNG accepted while Export callbacks lag "
                  "validation",
                  Fault::delayedExportCallbacks,
                  true,
                  true},
              std::tuple{
                  "reordered direct Export shares keep unique contributions",
                  Fault::reorderShares,
                  false,
                  true}})
        {
            if (!matches(label))
                continue;
            testcase(label);
            auto const selectedFault = fault;
            auto const enableRng = rng;
            auto const enableExport = exportOn;
            expectReplays(
                *this,
                label,
                [this, selectedFault, enableRng, enableExport](
                    SteppingNetwork& net) {
                    return scenario(
                        net, enableRng, enableExport, selectedFault);
                });
        }
        if (matches("overlapping Export origins stay distinct"))
        {
            testcase("overlapping Export origins stay distinct");
            expectReplays(
                *this,
                "overlapping Export origins stay distinct",
                [this](SteppingNetwork& net) {
                    return overlappingOrigins(net);
                });
        }
        if (matches("incomplete origin in global candidate"))
        {
            testcase("incomplete origin in global candidate");
            expectReplays(
                *this,
                "incomplete origin in global candidate",
                [this](SteppingNetwork& net) {
                    return incompleteOriginInCandidate(net);
                });
        }
        if (matches("observer restarts across Export-bearing history"))
        {
            testcase("observer restarts across Export-bearing history");
            expectReplays(
                *this,
                "observer restarts across Export-bearing history",
                [this](SteppingNetwork& net) {
                    return observerRestartAcrossExport(net);
                });
        }
        if (matches(
                "invalid Export share does not poison an honest contribution"))
        {
            testcase(
                "invalid Export share does not poison an honest contribution");
            expectReplays(
                *this,
                "invalid Export share does not poison an honest contribution",
                [this](SteppingNetwork& net) {
                    return invalidShareDoesNotPoison(net);
                });
        }
        if (matches(
                "no-quorum partition does not release Export before validated "
                "ancestry"))
        {
            testcase(
                "no-quorum partition does not release Export before validated "
                "ancestry");
            expectReplays(
                *this,
                "no-quorum partition does not release Export before validated "
                "ancestry",
                [this](SteppingNetwork& net) {
                    return noQuorumPartitionHeal(net);
                });
        }
        if (matches("publication window expiry rejects held Export material"))
        {
            testcase("publication window expiry rejects held Export material");
            expectReplays(
                *this,
                "publication window expiry rejects held Export material",
                [this](SteppingNetwork& net) {
                    return publicationWindowExpiry(net);
                });
        }
        if (matches(
                "candidate change inside the round deadline still witnesses "
                "once"))
        {
            testcase(
                "candidate change inside the round deadline still witnesses "
                "once");
            expectReplays(
                *this,
                "candidate change inside the round deadline still witnesses "
                "once",
                [this](SteppingNetwork& net) {
                    return candidateChangeInsideDeadline(net);
                });
        }
        // Known red: .ai-docs/reviews/2026-09-22-dsf-b2c-red-claude-review.md
        if (matches("post-deadline minority material still witnesses once"))
        {
            testcase("post-deadline minority material still witnesses once");
            expectReplays(
                *this,
                "post-deadline minority material still witnesses once",
                [this](SteppingNetwork& net) {
                    return lateMaterialAfterDeadline(net);
                });
        }
        BEAST_EXPECT(selected != 0);
    }
};

BEAST_DEFINE_TESTSUITE(SteppingExtensions, consensus, ripple);

}  // namespace ripple::test
