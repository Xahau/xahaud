#include <test/jtx/ExportPublication.h>
#include <test/jtx/MultiNode.h>
#include <test/jtx/pay.h>

#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/consensus/RCLConsensus.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/RuntimeConfig.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/rdb/RelationalDatabase.h>
#include <xrpld/ledger/View.h>
#include <xrpld/overlay/Message.h>
#include <xrpld/overlay/detail/ProtocolMessage.h>

#include <xrpl/basics/scope.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/ExportCommittee.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/ExportShare.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/TxFlags.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ripple::test {

class ThreadedExtensions_test : public beast::unit_test::suite
{
    static constexpr std::uint32_t networkID = 21337;
    static constexpr std::uint32_t observer = 3;
    static constexpr std::uint32_t nNodes = 4;

    struct Counts
    {
        std::atomic<std::uint32_t> lifetimeDirect{0};
        std::atomic<std::uint32_t> directCalls{0};
        std::atomic<std::uint32_t> directQueued{0};
        std::atomic<std::uint32_t> delayCalls{0};
        std::atomic<int> exportCategory{-1};
        std::atomic<int> delayNode{-1};
        std::atomic<bool> countDirect{false};
        std::atomic<bool> keysReady{false};
        std::array<std::atomic<std::uint32_t>, nNodes> own{};
        std::array<std::atomic<std::uint32_t>, nNodes> unauth{};
    };

    template <class T>
    static std::shared_ptr<T>
    decodeFrame(Message& message)
    {
        auto const& frame = message.getBuffer(compression::Compressed::Off);
        if (frame.empty())
            return {};
        auto const buffer = boost::asio::buffer(frame.data(), frame.size());
        boost::system::error_code ec;
        auto const header =
            ripple::detail::parseMessageHeader(ec, buffer, frame.size());
        if (ec || !header || header->total_wire_size != frame.size())
            return {};
        return ripple::detail::parseMessageContent<T>(*header, buffer);
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

    static std::uint32_t
    witnessAt(MultiNode& net, uint256 const& origin, std::uint32_t node)
    {
        if (!net.isLive(node))
            return 0;
        auto const last = net.validSeq(node);
        for (std::uint32_t seq = 1; seq <= last; ++seq)
        {
            auto const ledger = net.ledger(node, seq);
            if (!ledger)
                continue;
            for (auto const& [tx, meta] : ledger->txs)
                if (tx->getTxnType() == ttEXPORT_SIGNATURES &&
                    tx->getFieldH256(sfTransactionHash) == origin)
                    return seq;
        }
        return 0;
    }

    void
    realThreads()
    {
        testcase("export and rng under real threads");
        using namespace std::chrono_literals;

        Counts counts;
        std::array<std::optional<PublicKey>, nNodes> nodeKeys{};
        std::string schedule = "bringup";

        // Real job threads and real PeerImp, with the harness clock. A 1:1
        // wall clock would sit through the flag ledger; virtual time does not.
        MultiNode net(*this, /*virtualClock=*/true, /*stepping=*/false);
        auto const overlayFactory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        auto const configHook = [](Config& cfg) {
            cfg.NETWORK_ID = networkID;
            cfg.features.insert(featureNegativeUNL);
            cfg.features.insert(featureXahauGenesis);
            cfg.features.insert(featureConsensusEntropy);
            cfg.features.insert(featureExport);
        };

        auto const makeHook = [&](std::uint32_t id) {
            return [&, id](
                       std::uint16_t type,
                       std::string const&,
                       std::uint32_t,
                       beast::IP::Endpoint const&,
                       std::string const& stage,
                       Message& message) {
                try
                {
                    if (type == protocol::mtEXPORT_SHARES && stage == "call")
                    {
                        counts.lifetimeDirect.fetch_add(
                            1, std::memory_order_relaxed);
                        int expected = -1;
                        counts.exportCategory.compare_exchange_strong(
                            expected,
                            static_cast<int>(message.getCategory()),
                            std::memory_order_relaxed);
                        // Count this validator's direct frames. The sim
                        // remote port is not the observer's listen port, so
                        // a port filter never matches.
                        if (id == 2 &&
                            counts.countDirect.load(std::memory_order_relaxed))
                            counts.directCalls.fetch_add(
                                1, std::memory_order_relaxed);
                    }
                    if (id == 2 && type == protocol::mtEXPORT_SHARES &&
                        stage == "queued" &&
                        counts.countDirect.load(std::memory_order_relaxed))
                        counts.directQueued.fetch_add(
                            1, std::memory_order_relaxed);
                    if (stage == "call" &&
                        counts.delayNode.load(std::memory_order_relaxed) ==
                            static_cast<int>(id))
                        counts.delayCalls.fetch_add(
                            1, std::memory_order_relaxed);

                    if (stage != "call" ||
                        !counts.keysReady.load(std::memory_order_acquire) ||
                        !nodeKeys[id])
                        return;
                    auto const note = [&](ExportShare const& share) {
                        if (share.signingKey != *nodeKeys[id])
                            return;
                        counts.own[id].fetch_add(1, std::memory_order_relaxed);
                        if (!authorizedAtEmission(net[id].app(), share))
                            counts.unauth[id].fetch_add(
                                1, std::memory_order_relaxed);
                    };
                    if (type == protocol::mtEXPORT_SHARES)
                    {
                        auto const batch =
                            decodeFrame<protocol::TMExportShares>(message);
                        if (!batch)
                            return;
                        auto const parsed =
                            ripple::detail::parseExportShareBatch(*batch);
                        if (!parsed)
                            return;
                        for (auto const& share : *parsed)
                            note(share);
                    }
                    else if (type == protocol::mtPROPOSE_LEDGER)
                    {
                        auto const proposal =
                            decodeFrame<protocol::TMProposeSet>(message);
                        if (!proposal)
                            return;
                        for (auto const& bytes : proposal->exportsignatures())
                        {
                            auto const share =
                                ExportShare::parse(makeSlice(bytes));
                            if (share)
                                note(*share);
                        }
                    }
                }
                catch (...)
                {
                }
            };
        };

        std::vector<ValidatorKey> keys;
        std::vector<std::string> unl;
        for (auto const* name :
             {"thread-val-0", "thread-val-1", "thread-val-2"})
        {
            keys.push_back(ValidatorKey::fromPassphrase(name));
            unl.push_back(keys.back().pubKey);
        }
        auto const bring = [&](std::optional<TrustConfig> trust) {
            auto const id = static_cast<std::uint32_t>(net.size());
            net.add(
                std::move(trust),
                overlayFactory,
                {},
                /*bindServerListeners=*/false,
                [&](Config& cfg) { configHook(cfg); });
            return net.isLive(id);
        };
        for (auto const& key : keys)
            if (!bring(TrustConfig{key.seed, unl}))
            {
                log << "  threaded red bringup validator" << std::endl;
                BEAST_EXPECT(false);
                return;
            }
        if (!bring(TrustConfig{{}, unl}))
        {
            log << "  threaded red bringup observer" << std::endl;
            BEAST_EXPECT(false);
            return;
        }
        for (std::uint32_t id = 0; id < nNodes; ++id)
        {
            auto const& vk = net[id].app().getValidatorKeys();
            if (vk.keys)
            {
                nodeKeys[id] = vk.keys->publicKey;
            }
        }
        counts.keysReady.store(true, std::memory_order_release);

        auto const beat = [&]() {
            try
            {
                (void)net.threadedTick(1s);
                return true;
            }
            catch (std::exception const& e)
            {
                log << "  threaded red " << schedule << " tick: " << e.what()
                    << std::endl;
                BEAST_EXPECT(false);
                return false;
            }
        };
        auto const relink = [&](std::uint32_t id) {
            for (std::uint32_t i = 0; i < nNodes; ++i)
            {
                if (i == id || !net.isLive(i))
                    continue;
                if (!net.simConnect(std::min(i, id), std::max(i, id)))
                    return false;
            }
            return true;
        };
        for (std::uint32_t i = 0; i < nNodes; ++i)
            for (std::uint32_t j = i + 1; j < nNodes; ++j)
                if (!net.simConnect(i, j))
                {
                    log << "  threaded red sim link" << std::endl;
                    BEAST_EXPECT(false);
                    return;
                }
        if (!BEAST_EXPECT(net.waitForPeers(nNodes - 1, 20s)))
        {
            log << "  threaded red peers" << std::endl;
            return;
        }
        for (std::size_t i = 0; i < 120 && net.minValidated() < 3; ++i)
            if (!beat())
                return;
        if (net.minValidated() < 3)
        {
            log << "  threaded red initial validated=" << net.minValidated()
                << " closed=" << net.closedSeq(0) << "," << net.closedSeq(1)
                << "," << net.closedSeq(2) << "," << net.closedSeq(observer)
                << std::endl;
            BEAST_EXPECT(false);
            return;
        }

        jtx::Account owner{"thread-export-owner"};
        jtx::Account destination{"thread-export-destination"};
        auto const fail = [&](char const* why) {
            log << "  threaded red " << schedule << " " << why
                << " valid=" << net.minValidated()
                << " direct=" << counts.lifetimeDirect.load()
                << " calls=" << counts.directCalls.load()
                << " queued=" << counts.directQueued.load()
                << " delay=" << counts.delayCalls.load() << std::endl;
            BEAST_EXPECT(false);
        };
        auto const submitOk =
            [&](std::size_t node, Json::Value tx, jtx::Account const& signer) {
                tx[jss::NetworkID] = networkID;
                auto const txn = net.submit(node, std::move(tx), signer);
                return txn && txn->getResult() == tesSUCCESS;
            };
        if (!submitOk(
                0,
                jtx::pay(jtx::Account::master, owner, jtx::XRP(20'000)),
                jtx::Account::master))
            return fail("funding");
        {
            auto const funded = net.minValidated() + 2;
            for (std::size_t i = 0; i < 120 && net.minValidated() < funded; ++i)
                if (!beat())
                    return;
            if (net.minValidated() < funded)
                return fail("funding did not validate");
        }

        // The UNL report lands on a flag ledger. Virtual ticks reach it in
        // seconds; do not wait on the wall clock.
        schedule = "validator-view";
        auto const viewReady = [&] {
            auto const parent =
                net[0].app().getLedgerMaster().getValidatedLedger();
            if (!parent)
                return false;
            auto const view =
                net[0].app().getConsensusExtensions().makeActiveValidatorView(
                    parent);
            if (!view || !view->fromUNLReport)
                return false;
            for (std::uint32_t i = 0; i < observer; ++i)
            {
                auto const master =
                    net[i].app().getValidatorKeys().keys->masterPublicKey;
                if (!std::binary_search(
                        view->orderedOriginalMasterKeys.begin(),
                        view->orderedOriginalMasterKeys.end(),
                        master))
                    return false;
            }
            return net.minValidated() >= parent->seq();
        };
        // First reporting flag ledger is 256; the common active view is the
        // second window, 2*256+1. Beats per ledger vary with build speed
        // (about 6 on Release, above 12 on coverage), so the cap scales with
        // the ledgers still to close rather than fixing a beat count.
        constexpr std::uint32_t viewSeq = 2 * 256 + 2;
        constexpr std::size_t beatsPerLedger = 40;
        auto const remaining = viewSeq > net.minValidated()
            ? static_cast<std::size_t>(viewSeq - net.minValidated())
            : std::size_t{0};
        auto const viewBeats =
            std::max<std::size_t>(4500, remaining * beatsPerLedger);
        for (std::size_t i = 0; i < viewBeats && !viewReady(); ++i)
        {
            if (!beat())
                return;
            if (i % 256 == 0)
                log << "  threaded view wait valid=" << net.minValidated()
                    << std::endl;
        }
        if (!viewReady())
        {
            auto const parent =
                net[0].app().getLedgerMaster().getValidatedLedger();
            auto const view = parent
                ? net[0].app().getConsensusExtensions().makeActiveValidatorView(
                      parent)
                : nullptr;
            log << "  threaded view diag fromReport="
                << (view && view->fromUNLReport ? 1 : 0) << " masters="
                << (view ? view->orderedOriginalMasterKeys.size() : 0)
                << std::endl;
            return fail("validator view");
        }
        log << "  threaded view ready valid=" << net.minValidated()
            << std::endl;
        // The send hook cannot be installed on a live non-stepping node.
        // The report is already in the validated chain, so load that ledger
        // back with the hook in place before any intent is submitted.
        auto const hookedAt = net.minValidated();
        for (std::uint32_t id = 0; id < nNodes; ++id)
        {
            bool saved = false;
            for (int i = 0; i < 30; ++i)
            {
                auto const row =
                    net[id].app().getRelationalDatabase().getMaxLedgerSeq();
                if (row && *row >= net.validSeq(id))
                {
                    saved = true;
                    break;
                }
                if (!beat())
                    return;
            }
            if (!saved)
                return fail("ledger not saved");
        }
        for (std::uint32_t id = 0; id < nNodes; ++id)
        {
            net.stopNode(id);
            net.setPeerSendHook(id, makeHook(id));
            if (!net.restartNode(id).isUp())
                return fail("hook restart");
        }
        for (std::uint32_t i = 0; i < nNodes; ++i)
            for (std::uint32_t j = i + 1; j < nNodes; ++j)
                if (!net.simConnect(i, j))
                    return fail("hook link");
        for (int i = 0; i < 40 && !net.waitForPeers(nNodes - 1, 50ms); ++i)
            if (!beat())
                return;
        if (!net.waitForPeers(nNodes - 1, 2s))
            return fail("peers after hook");
        for (std::size_t i = 0; i < 200 && net.minValidated() < hookedAt; ++i)
            if (!beat())
                return;
        if (net.minValidated() < hookedAt || !viewReady())
            return fail("view lost after hook");

        auto intent = [&](std::uint32_t ticket, std::uint32_t lastLedger) {
            std::vector<PublicKey> rosterKeys;
            for (std::uint32_t i = 0; i < observer; ++i)
                rosterKeys.push_back(
                    net[i].app().getValidatorKeys().keys->masterPublicKey);
            auto const roster = canonicalizeExportCommittee(
                makeSlice(serializeExportCommittee(rosterKeys)));
            if (!roster)
                throw std::logic_error("invalid threaded committee");
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
            tx[jss::LastLedgerSequence] = lastLedger;
            tx[sfExportedTxn.jsonName] = inner.getJson(JsonOptions::none);
            tx[sfExportCommittee.jsonName] = strHex(*roster);
            tx[sfExportCommitteeHash.jsonName] =
                to_string(exportCommitteeHash(makeSlice(*roster)));
            return tx;
        };
        std::vector<uint256> origins;
        auto const historyFrom = net.minValidated();
        auto const sendIntent =
            [&](std::uint32_t ticket) -> std::optional<uint256> {
            auto const open = net[0].app().openLedger().current()->seq();
            Json::Value tx =
                intent(ticket, open + ExportLimits::maxAdmissionWindowLedgers);
            tx[jss::NetworkID] = networkID;
            auto const txn = net.submit(0, std::move(tx), owner);
            if (!txn || txn->getResult() != tesSUCCESS)
            {
                log << "  threaded red " << schedule << " intent " << ticket
                    << " result="
                    << (txn ? transHuman(txn->getResult()) : "null")
                    << std::endl;
                BEAST_EXPECT(false);
                return std::nullopt;
            }
            // One XAH cannot create an account. Pay the genesis account,
            // which already exists, so the payment is only load.
            auto pay = jtx::pay(owner, jtx::Account::master, jtx::XRP(1));
            pay[jss::NetworkID] = networkID;
            auto const paid = net.submit(0, std::move(pay), owner);
            if (!paid || paid->getResult() != tesSUCCESS)
            {
                log << "  threaded red " << schedule << " payment result="
                    << (paid ? transHuman(paid->getResult()) : "null")
                    << std::endl;
                BEAST_EXPECT(false);
                return std::nullopt;
            }
            return txn->getID();
        };
        auto const advance = [&](char const* why) {
            auto const target = net.minValidated() + 1;
            for (std::size_t i = 0; i < 200 && net.minValidated() < target; ++i)
                if (!beat())
                    return false;
            if (net.minValidated() < target)
            {
                fail(why);
                return false;
            }
            return true;
        };

        for (std::uint32_t ticket = 1; ticket <= 3; ++ticket)
        {
            auto const origin = sendIntent(ticket);
            if (!origin)
                return;
            origins.push_back(*origin);
            schedule = "intent " + std::to_string(ticket);
            if (!advance("intent did not validate"))
                return;
        }

        auto const preSeq = net.minValidated();
        auto const preHash = net.ledgerHash(0, preSeq);

        // The send hook is observe-only. The drop is the runtime peer fault
        // on the same send path, limited to the category the hook recorded.
        schedule = "direct-drop";
        auto const category = counts.exportCategory.load();
        counts.directCalls.store(0);
        counts.directQueued.store(0);
        counts.countDirect.store(true);
        {
            // Drop this validator's direct frames to every peer. The sim
            // remote address is not the observer listen port, so a single
            // peer key does not select the observer. A fresh intent is
            // submitted under the drop: the release is sent once.
            PeerFaultConfig cfg;
            cfg.sendDropPctX100 = 10000;
            if (category >= 0)
                cfg.messageCategories =
                    std::set<std::size_t>{static_cast<std::size_t>(category)};
            net[2].app().getRuntimeConfig().setPeerDefaults(cfg);
        }
        {
            auto const origin = sendIntent(4);
            if (!origin)
                return;
            origins.push_back(*origin);
        }
        for (int i = 0; i < 40 && counts.directCalls.load() == 0; ++i)
            if (!beat())
                return;
        auto const droppedCalls = counts.directCalls.load();
        auto const droppedQueued = counts.directQueued.load();
        counts.countDirect.store(false);
        net[2].app().getRuntimeConfig().clearPeerDefaults();
        if (droppedCalls == 0 || droppedQueued >= droppedCalls)
            return fail("direct counter");
        schedule += ":healed";
        if (!advance("no ledger after direct heal"))
            return;

        // The runtime send delay is an asio wall timer. Drop every frame for
        // a few virtual ticks instead, so a round of impairment is not a
        // wall-clock sleep.
        schedule += " all-drop";
        counts.delayCalls.store(0);
        counts.delayNode.store(1);
        {
            PeerFaultConfig cfg;
            cfg.sendDropPctX100 = 10000;
            net[1].app().getRuntimeConfig().setPeerDefaults(cfg);
        }
        for (int i = 0; i < 4; ++i)
            if (!beat())
                return;
        auto const delayed = counts.delayCalls.load();
        counts.delayNode.store(-1);
        net[1].app().getRuntimeConfig().clearPeerDefaults();
        if (delayed == 0)
            return fail("delay counter");
        schedule += ":healed";
        if (!advance("no ledger after delay heal"))
            return;

        for (std::uint32_t ticket = 5; ticket <= 6; ++ticket)
        {
            auto const origin = sendIntent(ticket);
            if (!origin)
                return;
            origins.push_back(*origin);
            if (!advance("later intent did not validate"))
                return;
        }

        schedule += " observer-stop";
        auto const downSeq = net.minValidated();
        {
            bool saved = false;
            for (int i = 0; i < 20; ++i)
            {
                auto const row = net[observer]
                                     .app()
                                     .getRelationalDatabase()
                                     .getMaxLedgerSeq();
                if (row && *row >= net.validSeq(observer))
                {
                    saved = true;
                    break;
                }
                if (!beat())
                    return;
            }
            if (!saved)
                return fail("observer ledger was not saved");
        }
        net.stopNode(observer);
        auto const validatorsAdvanced = [&] {
            std::uint32_t low = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t i = 0; i < observer; ++i)
                low = std::min(low, net.validSeq(i));
            return low;
        };
        for (int i = 0; i < 40 && validatorsAdvanced() < downSeq + 2; ++i)
            if (!beat())
                return;
        if (validatorsAdvanced() < downSeq + 2)
            return fail(
                "validators did not advance while the observer was down");
        if (!net.restartNode(observer).isUp())
            return fail("observer did not restart");
        if (!relink(observer))
            return fail("observer link");
        for (int i = 0; i < 20 && !net.waitForPeers(nNodes - 1, 50ms); ++i)
            if (!beat())
                return;
        if (!net.waitForPeers(nNodes - 1, 2s))
            return fail("peers after observer restart");
        schedule += ":healed";
        if (!advance("no ledger after observer heal"))
            return;

        auto const origin7 = sendIntent(7);
        if (!origin7)
            return;
        origins.push_back(*origin7);

        schedule += " validator-stop";
        auto const origin8 = sendIntent(8);
        if (!origin8)
            return;
        origins.push_back(*origin8);
        {
            bool saved = false;
            for (int i = 0; i < 20; ++i)
            {
                auto const row =
                    net[2].app().getRelationalDatabase().getMaxLedgerSeq();
                if (row && *row >= net.validSeq(2))
                {
                    saved = true;
                    break;
                }
                if (!beat())
                    return;
            }
            if (!saved)
                return fail("validator ledger was not saved");
        }
        net.stopNode(2);
        for (int i = 0; i < 3; ++i)
            if (!beat())
                return;
        if (net.isLive(2))
            return fail("validator was still live");
        if (!net.restartNode(2).isUp())
            return fail("validator did not restart");
        if (!relink(2))
            return fail("validator link");
        for (int i = 0; i < 20 && !net.waitForPeers(nNodes - 1, 50ms); ++i)
            if (!beat())
                return;
        if (!net.waitForPeers(nNodes - 1, 2s))
            return fail("peers after validator restart");
        schedule += ":healed";

        auto const target =
            net.minValidated() + ExportLimits::maxPublicationLedgers + 4;
        for (std::size_t i = 0; i < 80 && net.minValidated() < target; ++i)
            if (!beat())
                return;
        if (net.minValidated() < target)
            return fail("did not pass the publication window");
        {
            bool caughtUp = false;
            for (int i = 0; i < 80; ++i)
            {
                caughtUp = true;
                for (std::uint32_t seq = historyFrom; seq <= net.minValidated();
                     ++seq)
                    for (std::uint32_t n = 0; n < nNodes; ++n)
                        if (!net.ledger(n, seq))
                            caughtUp = false;
                if (caughtUp)
                    break;
                if (!beat())
                    return;
            }
            if (!caughtUp)
                return fail("history did not catch up");
        }

        auto const agreed = net.minValidated();
        for (std::uint32_t n = 0; n < nNodes; ++n)
            if (net.ledgerHash(n, preSeq) != preHash)
                return fail("pre-fault hash changed");
        if (!net.ledgersAgree(agreed) || !net.validatedForkFree())
            return fail("fork or disagreement");
        for (std::uint32_t n = 0; n < observer; ++n)
            if (counts.unauth[n].load() != 0)
                return fail("unauthorized emission");
        if (counts.own[observer].load() != 0 ||
            net[observer].app().getValidatorKeys().keys)
            return fail("observer authored");

        for (auto const& origin : origins)
        {
            auto const seqW = witnessAt(net, origin, 0);
            if (seqW != 0)
            {
                auto const canonical = net.ledger(0, seqW);
                if (!canonical)
                    return fail("missing witness ledger");
                std::vector<std::uint8_t> txBytes;
                std::vector<std::uint8_t> metaBytes;
                std::uint32_t hits = 0;
                for (auto const& [tx, meta] : canonical->txs)
                {
                    if (tx->getTxnType() != ttEXPORT_SIGNATURES ||
                        tx->getFieldH256(sfTransactionHash) != origin)
                        continue;
                    if (!meta)
                        return fail("witness without metadata");
                    ++hits;
                    txBytes = tx->getSerializer().getData();
                    metaBytes = meta->getSerializer().getData();
                }
                if (hits != 1)
                    return fail("witness count");
                for (std::uint32_t n = 1; n < nNodes; ++n)
                {
                    auto const ledger = net.ledger(n, seqW);
                    if (!ledger)
                        return fail("peer missing witness ledger");
                    bool found = false;
                    for (auto const& [tx, meta] : ledger->txs)
                    {
                        if (tx->getTxnType() != ttEXPORT_SIGNATURES ||
                            tx->getFieldH256(sfTransactionHash) != origin)
                            continue;
                        found = true;
                        if (!meta || tx->getSerializer().getData() != txBytes ||
                            meta->getSerializer().getData() != metaBytes)
                            return fail("witness bytes differ");
                    }
                    if (!found)
                        return fail("witness missing on a peer");
                }
                for (std::uint32_t seq = historyFrom; seq <= agreed; ++seq)
                {
                    if (seq == seqW)
                        continue;
                    auto const ledger = net.ledger(0, seq);
                    if (!ledger)
                        return fail("hole in validated history");
                    for (auto const& [tx, meta] : ledger->txs)
                        if (tx->getTxnType() == ttEXPORT_SIGNATURES &&
                            tx->getFieldH256(sfTransactionHash) == origin)
                            return fail("witnessed more than once");
                }
                log << "  threaded " << schedule
                    << " origin=" << to_string(origin) << " witnessed:" << seqW
                    << std::endl;
                continue;
            }
            for (std::uint32_t n = 0; n < nNodes; ++n)
                if (witnessAt(net, origin, n) != 0)
                    return fail("witness only on one node");
            auto const ledger = net.ledger(0, agreed);
            if (!ledger)
                return fail("missing tip");
            auto const latch =
                ledger->read(keylet::exportLatch(owner.id(), origin));
            // An expired latch stays, but a later successful intent prunes
            // it: sfExportNode is cleared and the pending directory drops
            // the key. The latch and its reserve remain, with no signature.
            auto const hasNode = latch && latch->isFieldPresent(sfExportNode);
            auto const hasSig =
                latch && latch->isFieldPresent(sfExportSignatureHash);
            auto const inDir =
                latch && pendingDirContains(*ledger, latch->key());
            char const* const shape = !latch ? "missing"
                : hasSig                     ? "signed"
                : hasNode && inDir           ? "pending"
                : !hasNode && !inDir         ? "pruned"
                                             : "inconsistent";
            log << "  threaded " << schedule << " origin=" << to_string(origin)
                << " expired shape=" << shape << std::endl;
            if (!latch)
                return fail("latch missing");
            if (!isExportPublicationExpired(*latch, ledger->seq()))
                return fail(
                    "unwitnessed origin has not passed its publication window");
            if (hasSig)
                return fail("expired latch has a signature hash");
            if (hasNode != inDir)
                return fail(
                    hasNode ? "pending link missing from directory"
                            : "directory holds an unlinked latch");
        }

        log << "  threaded " << schedule << " origins=" << origins.size()
            << " directDropped=" << droppedCalls - droppedQueued
            << " delayed=" << delayed << " observerDownFrom=" << downSeq
            << " pre=" << preSeq << " end=" << agreed << std::endl;
        BEAST_EXPECT(origins.size() == 8);
        BEAST_EXPECT(droppedCalls > droppedQueued);
        BEAST_EXPECT(delayed > 0);
        log << "  accept lock hold ns="
            << net[0].app().getOPs().getConsensus().maxAcceptLockHoldNs()
            << std::endl;
    }

    void
    testSimulateReenters()
    {
        testcase("simulate force-accept reenters the consensus lock");
        using namespace jtx;
        Env env{*this, envconfig()};
        auto& consensus = env.app().getOPs().getConsensus();
        consensus.simulate(
            env.app().timeKeeper().closeTime(), std::chrono::milliseconds{1});
        BEAST_EXPECT(consensus.maxAcceptLockHoldNs() > 0);
    }

    void
    testRestartClock()
    {
        testcase("threaded restart catches up 25 seconds before relinking");
        using namespace std::chrono_literals;
        MultiNode net(*this, /*virtualClock=*/true, /*stepping=*/false);
        auto const a = ValidatorKey::fromPassphrase("restart-clock-a");
        auto const b = ValidatorKey::fromPassphrase("restart-clock-b");
        std::vector<std::string> const unl{a.pubKey, b.pubKey};
        auto const factory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        auto const configure = [](Config& cfg) { cfg.NETWORK_ID = networkID; };
        for (auto const& seed : {a.seed, b.seed, std::string{}})
            net.add(TrustConfig{seed, unl}, factory, {}, false, configure);
        if (!BEAST_EXPECT(net.allUp()))
            return;
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = i + 1; j < 3; ++j)
                if (!BEAST_EXPECT(net.simConnect(i, j) != nullptr))
                    return;
        if (!BEAST_EXPECT(net.waitForPeers(2, 5s)))
            return;
        for (int i = 0; i < 120 && net.minValidated() < 3; ++i)
            (void)net.threadedTick(1s);
        if (!BEAST_EXPECT(net.minValidated() >= 3))
            return;

        constexpr std::size_t returning = 2;
        bool saved = false;
        for (int i = 0; i < 60; ++i)
        {
            auto const row =
                net[returning].app().getRelationalDatabase().getMaxLedgerSeq();
            if (row && *row >= 3)
            {
                saved = true;
                break;
            }
            (void)net.threadedTick(1s);
        }
        if (!BEAST_EXPECT(saved))
            return;

        auto const stoppedAt = net[returning].clock().now();
        net.stopNode(returning);
        for (int i = 0; i < 25; ++i)
            (void)net.threadedTick(1s);
        auto const expected = net[0].clock().now();
        BEAST_EXPECT(expected == stoppedAt + 25s);
        if (!BEAST_EXPECT(net.restartNode(returning).isUp()))
            return;
        log << "  restart clock: stopped="
            << stoppedAt.time_since_epoch().count()
            << " current=" << expected.time_since_epoch().count()
            << " restored="
            << net[returning].clock().now().time_since_epoch().count()
            << std::endl;
        BEAST_EXPECT(net[returning].clock().now() == expected);

        // Do not tick between restart and relink. Production handshake
        // verification must already see the current clock, not the saved one.
        bool linked = true;
        try
        {
            for (std::size_t i = 0; i < returning; ++i)
                linked = net.simConnect(i, returning) != nullptr && linked;
        }
        catch (std::exception const& ex)
        {
            log << "  restart clock relink: " << ex.what() << std::endl;
            linked = false;
        }
        if (!BEAST_EXPECT(linked))
            return;
        BEAST_EXPECT(net.waitForPeers(2, 5s));
    }

    void
    testAcceptLockWaits()
    {
        testcase("accept extension block waits while consensus holds the lock");
        using namespace std::chrono_literals;

        std::mutex mu;
        std::condition_variable cv;
        std::function<void()> pending;
        bool havePending = false;
        std::atomic<bool> capture{false};
        bool reaching = false;
        bool proceed = false;
        bool contended = false;
        bool completed = false;
        std::atomic<bool> rendezvousTimedOut{false};
        std::atomic<bool> entered{false};
        std::atomic<bool> sawWait{false};
        std::thread acceptThread;

        JobQueue::DispatchHook hook = [&](JobType type,
                                          std::string const&,
                                          JobQueue::JobFunction const& func) {
            if (type == jtACCEPT && capture.exchange(false))
            {
                std::lock_guard lock(mu);
                pending = func;
                havePending = true;
                return JobQueue::JobDisposition::claimedQueued;
            }
            return JobQueue::JobDisposition::pass;
        };

        MultiNode net(*this, /*virtualClock=*/true, /*stepping=*/false);
        // The captured job owns a JobCounter token. Release it before net's
        // destructor on every early return, or shutdown waits for this scope.
        scope_exit releasePending{[&] {
            capture.store(false);
            std::lock_guard lock(mu);
            pending = {};
        }};
        std::vector<ValidatorKey> keys;
        std::vector<std::string> unl;
        for (std::size_t i = 0; i < 3; ++i)
        {
            keys.push_back(ValidatorKey::fromPassphrase(
                "accept-lock-" + std::to_string(i)));
            unl.push_back(keys.back().pubKey);
        }
        auto const factory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        auto const configHook = [](Config& cfg) {
            cfg.NETWORK_ID = networkID;
            cfg.features.insert(featureConsensusEntropy);
            cfg.features.insert(featureExport);
        };
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            net.add(
                TrustConfig{keys[i].seed, unl},
                factory,
                i == 0 ? hook : JobQueue::DispatchHook{},
                /*bindServerListeners=*/false,
                configHook);
        }
        if (!BEAST_EXPECT(net.allUp()))
            return;
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = i + 1; j < 3; ++j)
                if (!BEAST_EXPECT(net.simConnect(i, j) != nullptr))
                    return;
        if (!BEAST_EXPECT(net.waitForPeers(2, 20s)))
            return;

        auto& consensus = net[0].app().getOPs().getConsensus();
        scope_exit clearProbes{[&] {
            consensus.setWhileConsensusLocked({});
            consensus.setAcceptExtensionProbe({}, {});
        }};
        consensus.setAcceptExtensionProbe(
            [&] {
                std::unique_lock lock(mu);
                reaching = true;
                cv.notify_all();
                if (!cv.wait_for(lock, 5s, [&] { return proceed; }))
                    rendezvousTimedOut.store(true);
            },
            [&] { entered.store(true, std::memory_order_release); },
            [&] {
                std::lock_guard lock(mu);
                contended = true;
                cv.notify_all();
            });

        capture.store(true, std::memory_order_release);
        for (int i = 0; i < 40; ++i)
        {
            (void)net.threadedTick(1s);
            std::lock_guard lock(mu);
            if (havePending)
                break;
        }
        capture.store(false, std::memory_order_release);
        std::function<void()> job;
        {
            std::lock_guard lock(mu);
            if (!BEAST_EXPECT(havePending))
                return;
            job = std::move(pending);
            havePending = false;
        }
        // Let the first two accept-time acquisitions finish before holding C.
        // The worker rendezvous is immediately before the onPreBuild lock.
        acceptThread = std::thread([&, job = std::move(job)] {
            job();
            std::lock_guard lock(mu);
            completed = true;
            cv.notify_all();
        });
        {
            std::unique_lock lock(mu);
            if (!cv.wait_for(lock, 5s, [&] { return reaching; }))
                rendezvousTimedOut.store(true);
        }
        consensus.setWhileConsensusLocked([&] {
            std::unique_lock lock(mu);
            proceed = true;
            cv.notify_all();
            auto const blocked =
                cv.wait_for(lock, 5s, [&] { return contended; });
            if (!blocked)
                rendezvousTimedOut.store(true);
            sawWait.store(blocked && !entered.load(std::memory_order_acquire));
        });
        consensus.timerEntry(net[0].app().timeKeeper().closeTime(), {});
        consensus.setWhileConsensusLocked({});
        // C has been released. A broken rendezvous must fail, not strand the
        // accept thread or its JobCounter token during Application teardown.
        {
            std::unique_lock lock(mu);
            if (!cv.wait_for(lock, 30s, [&] { return completed; }))
            {
                BEAST_EXPECT(false);
                log << "accept job did not finish after C was released"
                    << std::endl;
                std::abort();
            }
        }
        acceptThread.join();
        BEAST_EXPECT(!rendezvousTimedOut.load());
        BEAST_EXPECT(sawWait.load(std::memory_order_acquire));
        BEAST_EXPECT(entered.load(std::memory_order_acquire));
        log << "  accept lock hold ns=" << consensus.maxAcceptLockHoldNs()
            << std::endl;
    }

public:
    void
    run() override
    {
        if (arg() == "restart-clock-only")
        {
            testRestartClock();
            return;
        }
        testSimulateReenters();
        testAcceptLockWaits();
        testRestartClock();
        realThreads();
    }
};

BEAST_DEFINE_TESTSUITE(ThreadedExtensions, consensus, ripple);

}  // namespace ripple::test
