#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>
#include <test/jtx/pay.h>

#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/protocol/ExportCommittee.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/TxFlags.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
        std::size_t candidateLeavesA = 0;
        std::size_t candidateLeavesB = 0;
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

    static std::size_t
    originSidecarLeaves(SHAMap const& map, uint256 const& origin)
    {
        std::size_t n = 0;
        map.visitLeaves(
            [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                try
                {
                    SerialIter sit{item->slice()};
                    STObject const obj{sit, sfGeneric};
                    if (obj.isFieldPresent(sfTransactionHash) &&
                        obj.getFieldH256(sfTransactionHash) == origin)
                        ++n;
                }
                catch (...)
                {
                }
            });
        return n;
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
                cfg.harnessPeerMessage =
                    [stats, id](
                        std::uint16_t type,
                        std::string const&,
                        std::uint32_t,
                        beast::IP::Endpoint const&,
                        ::google::protobuf::Message const& msg) {
                        if (type == protocol::mtEXPORT_SHARES)
                        {
                            ++stats->directFrames.at(id);
                            if (id == observer)
                            {
                                auto const& batch = static_cast<
                                    protocol::TMExportShares const&>(msg);
                                stats->shareRecvOrder.push_back(
                                    directBatchId(batch));
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
                            SerialIter iter{
                                makeSlice(proposal.currenttxhash())};
                            auto const position =
                                ExtendedPosition::fromSerialIter(
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
    witnessAt(SteppingNetwork& net, uint256 const& origin, std::uint32_t from)
    {
        for (auto seq = from; seq <= net.validSeq(observer); ++seq)
            if (auto const ledger = net.ledger(observer, seq))
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
                    [stats, from](
                        std::uint16_t type, SimPipe::Frame bytes) {
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
        auto const longWindow =
            open + ExportLimits::maxAdmissionWindowLedgers;
        auto const shortWindow = open + 3;
        std::vector<uint256> origins;
        auto const submitIntent =
            [&](jtx::Account const& acct,
                std::uint32_t ticket,
                std::uint32_t lastLedger) {
                auto const tx = world.submit(
                    observer,
                    world.intent(acct, ticket, lastLedger),
                    acct);
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
                outcome.push_back(sha512Half(makeSlice(txBytes), makeSlice(metaBytes)));
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
                    {seq,
                     net.ledgerHash(0, seq - 1),
                     net.ledgerHash(0, seq)}));
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
        net.faultFrames(
            2,
            observer,
            [stats, originB](std::uint16_t type, SimPipe::Frame bytes) {
                SimFault f;
                auto const hit = [&](std::string const& blob) {
                    auto const share = ExportShare::parse(makeSlice(blob));
                    return share && share->originTxn == originB;
                };
                if (type == protocol::mtEXPORT_SHARES)
                {
                    auto const batch =
                        decodeFrame<protocol::TMExportShares>(bytes);
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
            });
        net.controller().observeJobs(
            [&net, stats, originA, originB](
                std::uint32_t id, JobType, std::string const&) {
                if (id != observer || !net.isLive(id) ||
                    stats->sawPartialCandidate)
                    return;
                auto& ce = net.node(id).app().getConsensusExtensions();
                if (!ce.roundParentLedger_ || !ce.exportSigSetMap_)
                    return;
                auto const& parent = *ce.roundParentLedger_;
                auto const pending =
                    ce.pendingRoundExports(parent.info().seq + 1);
                if (!pending.contains(originA) || !pending.contains(originB))
                    return;
                auto const leavesA =
                    originSidecarLeaves(*ce.exportSigSetMap_, originA);
                auto const leavesB =
                    originSidecarLeaves(*ce.exportSigSetMap_, originB);
                auto const needA =
                    ExportLimits::committeeQuorumThreshold(2);
                auto const needB =
                    ExportLimits::committeeQuorumThreshold(3);
                if (leavesA == needA && leavesB > 0 && leavesB < needB)
                {
                    stats->sawPartialCandidate = true;
                    stats->candidateLeavesA = leavesA;
                    stats->candidateLeavesB = leavesB;
                }
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
            << " droppedDirect=" << stats->droppedStarvedDirect
            << " droppedProposals=" << stats->droppedStarvedProposals
            << std::endl;

        net.faultFrames(2, observer, {});
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
        if (seqA)
            BEAST_EXPECT(stats->builds[observer].contains(
                {seqA,
                 net.ledgerHash(0, seqA - 1),
                 net.ledgerHash(0, seqA)}));
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
        BEAST_EXPECT(selected != 0);
    }
};

BEAST_DEFINE_TESTSUITE(SteppingExtensions, consensus, ripple);

}  // namespace ripple::test
