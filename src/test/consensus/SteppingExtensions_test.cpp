#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>
#include <test/jtx/pay.h>

#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/ExportCommittee.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/TxFlags.h>

#include <algorithm>
#include <array>
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
        slowValidatorIdleExport
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
        std::uint32_t droppedDirect = 0;
        std::uint32_t droppedProposals = 0;
        std::uint32_t duplicatedFrames = 0;
        std::uint32_t delayedValidations = 0;
        std::uint32_t delayedValidatorTraffic = 0;
        std::uint32_t validationGap = 0;
        std::uint32_t validatedAheadOfClosed = 0;
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
                            ++stats->directFrames.at(id);
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
                    [stats = observed, id, prior](
                        std::uint16_t type,
                        std::string const& name,
                        std::uint32_t peer,
                        beast::IP::Endpoint const& remote,
                        std::string const& stage,
                        Message& message) {
                        if (prior)
                            prior(type, name, peer, remote, stage, message);
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
        intent(std::uint32_t ticket = 1)
        {
            std::vector<PublicKey> keys;
            for (std::uint32_t i = 0; i < observer; ++i)
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
            inner.setAccountID(sfAccount, owner.id());
            inner.setAccountID(sfDestination, destination.id());

            Json::Value tx;
            tx[jss::TransactionType] = jss::Export;
            tx[jss::Account] = owner.human();
            tx[jss::Fee] = "1000000";
            tx[jss::LastLedgerSequence] =
                net.node(observer).app().openLedger().current()->seq() +
                ExportLimits::maxAdmissionWindowLedgers;
            tx[sfExportedTxn.jsonName] = inner.getJson(JsonOptions::none);
            tx[sfExportCommittee.jsonName] = strHex(*roster);
            tx[sfExportCommitteeHash.jsonName] =
                to_string(exportCommitteeHash(makeSlice(*roster)));
            return tx;
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
            net.in(16s, observer, [&net, late, sluggish]() {
                net.clearLag(observer).clearLag(1);
                if (late)
                    for (std::uint32_t from = 0; from < observer; ++from)
                        net.faultLink(from, observer, {});
                if (sluggish)
                    for (std::uint32_t to = 0; to <= observer; ++to)
                        if (to != 1)
                            net.faultLink(1, to, {});
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
            SteppingNetwork::Cadence{sluggish ? 250ms : 1000ms});
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
            fault == Fault::duplicateTraffic || fault == Fault::slowObserver)
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
        for (std::uint32_t i = 0; i < observer; ++i)
            BEAST_EXPECT((world.observed->secrets[i] != 0) == rng);

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
            << std::endl;
        outcome.push_back(sha512Half(
            stats->droppedDirect,
            stats->droppedProposals,
            stats->duplicatedFrames,
            stats->delayedValidations,
            stats->delayedValidatorTraffic,
            stats->validationGap,
            stats->validatedAheadOfClosed,
            localBuilds,
            localMismatches));
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
        BEAST_EXPECT(selected != 0);
    }
};

BEAST_DEFINE_TESTSUITE(SteppingExtensions, consensus, ripple);

}  // namespace ripple::test
