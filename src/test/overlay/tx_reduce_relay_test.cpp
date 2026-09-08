//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright 2020 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================
#include <test/jtx.h>
#include <test/jtx/Env.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/overlay/detail/OverlayImpl.h>
#include <xrpld/overlay/detail/PeerImp.h>
#include <xrpld/overlay/detail/ProtocolMessage.h>
#include <xrpld/peerfinder/detail/SlotImp.h>
#include <xrpl/basics/base64.h>
#include <xrpl/basics/make_SSLContext.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/Sign.h>

#include <condition_variable>
#include <future>

namespace ripple {

namespace test {

class tx_reduce_relay_test : public beast::unit_test::suite
{
public:
    using socket_type = boost::asio::ip::tcp::socket;
    using middle_type = boost::beast::tcp_stream;
    using stream_type = boost::beast::ssl_stream<middle_type>;
    using shared_context = std::shared_ptr<boost::asio::ssl::context>;

private:
    static std::string
    signedManifest(
        std::pair<PublicKey, SecretKey> const& master,
        std::pair<PublicKey, SecretKey> const& signer,
        std::uint32_t seq)
    {
        STObject st{sfGeneric};
        st[sfSequence] = seq;
        st[sfPublicKey] = master.first;
        if (seq != std::numeric_limits<std::uint32_t>::max())
        {
            st[sfSigningPubKey] = signer.first;
            sign(st, HashPrefix::manifest, KeyType::secp256k1, signer.second);
        }
        sign(
            st,
            HashPrefix::manifest,
            KeyType::ed25519,
            master.second,
            sfMasterSignature);
        return st.getSerializer().getString();
    }

    void
    doTest(const std::string& msg, bool log, std::function<void(bool)> f)
    {
        testcase(msg);
        f(log);
    }

    void
    testConfig(bool log)
    {
        doTest("Config Test", log, [&](bool log) {
            auto test = [&](bool enable,
                            bool metrics,
                            std::uint16_t min,
                            std::uint16_t pct,
                            bool success = true) {
                std::stringstream str("[reduce_relay]");
                str << "[reduce_relay]\n"
                    << "tx_enable=" << static_cast<int>(enable) << "\n"
                    << "tx_metrics=" << static_cast<int>(metrics) << "\n"
                    << "tx_min_peers=" << min << "\n"
                    << "tx_relay_percentage=" << pct << "\n";
                Config c;
                try
                {
                    c.loadFromString(str.str());

                    BEAST_EXPECT(c.TX_REDUCE_RELAY_ENABLE == enable);
                    BEAST_EXPECT(c.TX_REDUCE_RELAY_METRICS == metrics);
                    BEAST_EXPECT(c.TX_REDUCE_RELAY_MIN_PEERS == min);
                    BEAST_EXPECT(c.TX_RELAY_PERCENTAGE == pct);
                    if (success)
                        pass();
                    else
                        fail();
                }
                catch (...)
                {
                    if (success)
                        fail();
                    else
                        pass();
                }
            };

            test(true, true, 20, 25);
            test(false, false, 20, 25);
            test(false, false, 20, 0, false);
            test(false, false, 20, 101, false);
            test(false, false, 9, 10, false);
            test(false, false, 10, 9, false);
        });
    }

    class PeerTest : public PeerImp
    {
    public:
        PeerTest(
            Application& app,
            std::shared_ptr<PeerFinder::Slot> const& slot,
            http_request_type&& request,
            PublicKey const& publicKey,
            ProtocolVersion protocol,
            Resource::Consumer consumer,
            std::unique_ptr<tx_reduce_relay_test::stream_type>&& stream_ptr,
            OverlayImpl& overlay)
            : PeerImp(
                  app,
                  sid_,
                  slot,
                  std::move(request),
                  publicKey,
                  protocol,
                  consumer,
                  std::move(stream_ptr),
                  overlay)
        {
            sid_++;
        }
        ~PeerTest() = default;

        template <class Buffers>
        PeerTest(
            Application& app,
            std::unique_ptr<tx_reduce_relay_test::stream_type>&& stream,
            Buffers const& buffers,
            std::shared_ptr<PeerFinder::Slot>&& slot,
            Resource::Consumer consumer,
            PublicKey const& key,
            ProtocolVersion protocol,
            OverlayImpl& overlay)
            : PeerImp(
                  app,
                  std::move(stream),
                  buffers,
                  std::move(slot),
                  http_response_type{},
                  consumer,
                  key,
                  protocol,
                  sid_++,
                  overlay)
            , protocolRun_(true)
        {
        }

        void
        charge(Resource::Charge const& fee, std::string const& context) override
        {
            PeerImp::charge(fee, context);
            if (context == "manifest intake")
            {
                std::lock_guard lock{chargeMutex_};
                ++batchesCharged_;
                chargeReady_.notify_all();
            }
        }

        bool
        waitCharges(unsigned batches)
        {
            std::unique_lock lock{chargeMutex_};
            return chargeReady_.wait_for(lock, std::chrono::seconds{5}, [&]() {
                return batchesCharged_ >= batches;
            });
        }

        std::mutex chargeMutex_;
        std::condition_variable chargeReady_;
        unsigned batchesCharged_ = 0;
        unsigned packetsSent_ = 0;
        bool protocolRun_ = false;

        bool
        waitSent(unsigned packets)
        {
            std::unique_lock lock{chargeMutex_};
            return chargeReady_.wait_for(lock, std::chrono::seconds{5}, [&]() {
                return packetsSent_ >= packets;
            });
        }

        void
        run() override
        {
            if (protocolRun_)
                PeerImp::run();
        }
        void
        send(std::shared_ptr<Message> const&) override
        {
            sendTx_++;
            std::lock_guard lock{chargeMutex_};
            ++packetsSent_;
            chargeReady_.notify_all();
        }
        void
        addTxQueue(const uint256& hash) override
        {
            queueTx_++;
        }
        static void
        init()
        {
            queueTx_ = 0;
            sendTx_ = 0;
            sid_ = 0;
        }
        inline static std::size_t sid_ = 0;
        inline static std::uint16_t queueTx_ = 0;
        inline static std::atomic<std::uint16_t> sendTx_{0};
    };

    std::uint16_t lid_{0};
    std::uint16_t rid_{1};
    shared_context context_;
    ProtocolVersion protocolVersion_;
    boost::beast::multi_buffer read_buf_;

public:
    tx_reduce_relay_test()
        : context_(make_SSLContext("")), protocolVersion_{1, 7}
    {
    }

private:
    void
    addPeer(
        jtx::Env& env,
        std::vector<std::shared_ptr<PeerTest>>& peers,
        std::uint16_t& nDisabled)
    {
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        boost::beast::http::request<boost::beast::http::dynamic_body> request;
        (nDisabled == 0)
            ? (void)request.insert(
                  "X-Protocol-Ctl",
                  makeFeaturesRequestHeader(false, false, true, false))
            : (void)nDisabled--;
        auto stream_ptr = std::make_unique<stream_type>(
            socket_type(std::forward<boost::asio::io_service&>(
                env.app().getIOService())),
            *context_);
        beast::IP::Endpoint local(
            beast::IP::Address::from_string("172.1.1." + std::to_string(lid_)));
        beast::IP::Endpoint remote(
            beast::IP::Address::from_string("172.1.1." + std::to_string(rid_)));
        PublicKey key(std::get<0>(randomKeyPair(KeyType::ed25519)));
        auto consumer = overlay.resourceManager().newInboundEndpoint(remote);
        auto slot = overlay.peerFinder().new_inbound_slot(local, remote);
        auto const peer = std::make_shared<PeerTest>(
            env.app(),
            slot,
            std::move(request),
            key,
            protocolVersion_,
            consumer,
            std::move(stream_ptr),
            overlay);
        BEAST_EXPECT(
            overlay.findPeerByPublicKey(key) == std::shared_ptr<PeerImp>{});
        overlay.add_active(peer);
        BEAST_EXPECT(overlay.findPeerByPublicKey(key) == peer);
        peers.emplace_back(peer);  // overlay stores week ptr to PeerImp
        lid_ += 2;
        rid_ += 2;
        assert(lid_ <= 254);
    }

    void
    testRelay(
        std::string const& test,
        bool txRREnabled,
        std::uint16_t nPeers,
        std::uint16_t nDisabled,
        std::uint16_t minPeers,
        std::uint16_t relayPercentage,
        std::uint16_t expectRelay,
        std::uint16_t expectQueue,
        std::set<Peer::id_t> const& toSkip = {})
    {
        testcase(test);
        jtx::Env env(*this);
        std::vector<std::shared_ptr<PeerTest>> peers;
        env.app().config().TX_REDUCE_RELAY_ENABLE = txRREnabled;
        env.app().config().TX_REDUCE_RELAY_MIN_PEERS = minPeers;
        env.app().config().TX_RELAY_PERCENTAGE = relayPercentage;
        PeerTest::init();
        lid_ = 0;
        rid_ = 0;
        for (int i = 0; i < nPeers; i++)
            addPeer(env, peers, nDisabled);

        auto const jtx = env.jt(noop(env.master));
        if (BEAST_EXPECT(jtx.stx))
        {
            protocol::TMTransaction m;
            Serializer s;
            jtx.stx->add(s);
            m.set_rawtransaction(s.data(), s.size());
            m.set_deferred(false);
            m.set_status(protocol::TransactionStatus::tsNEW);
            env.app().overlay().relay(uint256{0}, m, toSkip);
            BEAST_EXPECT(
                PeerTest::sendTx_ == expectRelay &&
                PeerTest::queueTx_ == expectQueue);
        }
    }

    void
    testManifestIngress()
    {
        testcase("bounded manifest ingress and job backlog");
        jtx::Env env{*this};
        std::vector<std::shared_ptr<PeerTest>> peers;
        std::uint16_t disabled = 0;
        lid_ = 0;
        rid_ = 1;
        addPeer(env, peers, disabled);
        auto const peer = peers.front();
        auto& cache = env.app().validatorManifests();
        std::vector<PublicKey> masters;
        std::vector<std::string> blobs;
        for (int i = 0; i < 5; ++i)
        {
            auto const master = randomKeyPair(KeyType::ed25519);
            auto const signer = randomKeyPair(KeyType::secp256k1);
            blobs.push_back(signedManifest(master, signer, 1));
            masters.push_back(master.first);
        }
        cache.pin(hash_set<PublicKey>(masters.begin(), masters.end()));
        auto packet = [&](int key, int n = 1) {
            auto m = std::make_shared<protocol::TMManifests>();
            for (int i = 0; i < n; ++i)
                m->add_list()->set_stobject(blobs[key]);
            return m;
        };
        auto send = [&](auto const& m) {
            peer->onMessageBegin(
                protocol::mtMANIFESTS,
                m,
                m->ByteSizeLong(),
                m->ByteSizeLong(),
                false);
            peer->onMessage(m);
            peer->onMessageEnd(protocol::mtMANIFESTS, m);
        };
        send(packet(0, maxManifestEntries + 1));
        auto oversized = packet(0);
        oversized->add_list()->set_stobject(
            std::string(maxManifestSize + 1, 'x'));
        send(oversized);
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(!cache.getRawManifest(masters[0]));
        send(packet(0, maxManifestEntries));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(cache.getRawManifest(masters[0]));

        send(packet(1));
        send(packet(2));
        send(packet(3));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(cache.getRawManifest(masters[1]));
        BEAST_EXPECT(cache.getRawManifest(masters[2]));
        BEAST_EXPECT(cache.getRawManifest(masters[3]));

        // The shared signature backlog has an overload cutoff; it does not
        // turn an ordinary three-packet connect burst into a per-peer error.
        std::promise<void> release;
        auto gate = release.get_future().share();
        for (int i = 0; i < maxManifestJobs; ++i)
            BEAST_EXPECT(env.app().getJobQueue().addJob(
                jtMANIFEST, "hold", [gate]() { gate.wait(); }));
        send(packet(4));
        BEAST_EXPECT(!cache.getRawManifest(masters[4]));
        release.set_value();
        env.app().getJobQueue().rendezvous();

        auto invalid = packet(4);
        invalid->mutable_list(0)->mutable_stobject()->back() ^= 1;
        send(invalid);
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(!cache.getRawManifest(masters[4]));
        send(packet(4));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(cache.getRawManifest(masters[4]));

        // Completion must charge on the strand: an overloaded peer really
        // disconnects, instead of merely accumulating an off-thread balance.
        BEAST_EXPECT(peer->waitCharges(7));
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto consumer = overlay.resourceManager().newInboundEndpoint(
            peer->getRemoteAddress());
        consumer.charge(Resource::feeDrop * 200);
        auto const drops = overlay.getPeerDisconnectCharges();
        send(packet(4));
        BEAST_EXPECT(peer->waitCharges(8));
        BEAST_EXPECT(overlay.getPeerDisconnectCharges() == drops + 1);
    }

    void
    testManifestCapacityRelay()
    {
        testcase("full cache preserves admission and relay history");
        jtx::Env env{*this};
        auto& cache = env.app().validatorManifests();
        auto make = [](auto const& master, std::uint32_t seq) {
            return signedManifest(
                master, randomKeyPair(KeyType::secp256k1), seq);
        };
        std::vector<std::shared_ptr<PeerTest>> peers;
        std::uint16_t disabled = 3;
        PeerTest::init();
        PeerTest::sid_ = 1;
        lid_ = 0;
        rid_ = 1;
        for (int i = 0; i < 3; ++i)
            addPeer(env, peers, disabled);
        auto send = [&](std::string const& blob) {
            auto packet = std::make_shared<protocol::TMManifests>();
            packet->add_list()->set_stobject(blob);
            auto const& peer = peers.front();
            peer->onMessageBegin(
                protocol::mtMANIFESTS,
                packet,
                packet->ByteSizeLong(),
                packet->ByteSizeLong(),
                false);
            peer->onMessage(packet);
            peer->onMessageEnd(protocol::mtMANIFESTS, packet);
            env.app().getJobQueue().rendezvous();
        };
        auto const master = randomKeyPair(KeyType::ed25519);
        auto const first = make(master, 1);
        cache.pin({master.first});
        send(first);
        auto const sentInitially = PeerTest::sendTx_.load();
        BEAST_EXPECT(sentInitially >= 2);
        BEAST_EXPECT(cache.getSequence(master.first) == 1);

        // Fill the remaining capacity, then attempt one extra identity. The
        // original row must remain, so replay cannot restart its broadcast.
        for (std::size_t i = 0; i < ManifestCache::cacheLimit; ++i)
        {
            auto manifest =
                deserializeManifest(make(randomKeyPair(KeyType::ed25519), 1));
            BEAST_EXPECT(
                cache.applyManifest(std::move(*manifest)) ==
                (i + 1 < ManifestCache::cacheLimit
                     ? ManifestDisposition::accepted
                     : ManifestDisposition::full));
        }
        BEAST_EXPECT(cache.getRawManifest(master.first));
        send(first);
        BEAST_EXPECT(cache.getSequence(master.first) == 1);
        BEAST_EXPECT(PeerTest::sendTx_ == sentInitially);

        // A real rotation is a different signed manifest and still propagates.
        send(make(master, 2));
        BEAST_EXPECT(cache.getSequence(master.first) == 2);
        BEAST_EXPECT(PeerTest::sendTx_ == sentInitially * 2);
        BEAST_EXPECT(sentInitially == 2);  // Do not echo back to the sender.
        BEAST_EXPECT(peers.front()->waitCharges(3));
    }

    void
    testListedGossip()
    {
        testcase("only local-list gossip enters and leaves the cache");
        jtx::Env env{*this};
        auto& cache = env.app().validatorManifests();
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const master = randomKeyPair(KeyType::ed25519);
        auto const signer = randomKeyPair(KeyType::secp256k1);
        auto make = [&](std::uint32_t seq) {
            return signedManifest(master, signer, seq);
        };
        std::vector<std::shared_ptr<PeerTest>> peers;
        PeerTest::init();
        PeerTest::sid_ = 1;
        lid_ = 0;
        rid_ = 1;
        std::uint16_t disabled = 3;
        for (int i = 0; i < 3; ++i)
            addPeer(env, peers, disabled);
        auto send = [&](std::string const& blob) {
            auto m = std::make_shared<protocol::TMManifests>();
            m->add_list()->set_stobject(blob);
            peers.front()->onMessage(m);
            env.app().getJobQueue().rendezvous();
        };
        auto const first = make(1);
        send(first);
        BEAST_EXPECT(!cache.getRawManifest(master.first));
        BEAST_EXPECT(PeerTest::sendTx_ == 0);
        BEAST_EXPECT(overlay.getManifestsMessages().empty());

        // Even a warm on-ledger-style mapping does not admit its gossip.
        BEAST_EXPECT(
            cache.applyManifest(*deserializeManifest(first)) ==
            ManifestDisposition::accepted);
        send(make(2));
        BEAST_EXPECT(cache.getSequence(master.first) == 1);
        BEAST_EXPECT(PeerTest::sendTx_ == 0);
        BEAST_EXPECT(overlay.getManifestsMessages().empty());

        cache.pin({master.first});
        BEAST_EXPECT(!overlay.getManifestsMessages().empty());
        // A listed rotation must use a fresh signing key (key-role rule).
        auto const revocation = make(std::numeric_limits<std::uint32_t>::max());
        send(revocation);
        BEAST_EXPECT(cache.revoked(master.first));
        BEAST_EXPECT(PeerTest::sendTx_ == 2);
        auto listedMessages = overlay.getManifestsMessages();
        BEAST_EXPECT(!listedMessages.empty());

        cache.pin({});
        BEAST_EXPECT(overlay.getManifestsMessages().empty());
        send(revocation);
        BEAST_EXPECT(PeerTest::sendTx_ == 2);
        BEAST_EXPECT(cache.revoked(master.first));
        BEAST_EXPECT(peers.front()->waitCharges(4));
    }

    void
    testManifestBatches()
    {
        testcase("all local manifests cross more than two coalesced packets");
        jtx::Env sender{*this};
        jtx::Env receiver{*this};
        auto& source = sender.app().validatorManifests();
        auto& target = receiver.app().validatorManifests();
        auto& outbound = dynamic_cast<OverlayImpl&>(sender.app().overlay());
        auto& inbound = dynamic_cast<OverlayImpl&>(receiver.app().overlay());
        hash_set<PublicKey> keys;
        std::vector<std::string> blobs;
        for (int i = 0; i < 2 * maxManifestEntries + 1; ++i)
        {
            auto const master = randomKeyPair(KeyType::ed25519);
            keys.insert(master.first);
            blobs.push_back(
                signedManifest(master, randomKeyPair(KeyType::secp256k1), 1));
        }
        source.pin(keys);
        for (auto const& blob : blobs)
            BEAST_EXPECT(
                source.applyManifest(*deserializeManifest(blob)) ==
                ManifestDisposition::accepted);
        auto const own = randomKeyPair(KeyType::ed25519);
        auto const revocation = signedManifest(
            own,
            randomKeyPair(KeyType::secp256k1),
            std::numeric_limits<std::uint32_t>::max());
        BEAST_EXPECT(source.loadConfig({}, {base64_encode(revocation)}));
        keys.insert(own.first);
        target.pin(keys);
        // Seed one row so the outgoing-peer constructor's initial send also
        // tells this test that its preloaded receive buffer has been handled.
        BEAST_EXPECT(
            target.applyManifest(*deserializeManifest(blobs.front())) ==
            ManifestDisposition::accepted);
        auto const packets = outbound.getManifestsMessages();
        BEAST_EXPECT(packets.size() == 3);
        std::vector<std::uint8_t> wire;
        hash_set<PublicKey> offered;
        for (auto const& packet : packets)
        {
            auto const& bytes = packet->getBuffer(compression::Compressed::Off);
            boost::system::error_code ec;
            auto const header = detail::parseMessageHeader(
                ec, boost::asio::buffer(bytes), bytes.size());
            if (!BEAST_EXPECT(header && !ec))
                return;
            BEAST_EXPECT(header->payload_wire_size <= maxManifestMessageSize);
            auto parsed = detail::parseMessageContent<protocol::TMManifests>(
                *header, boost::asio::buffer(bytes));
            if (!BEAST_EXPECT(parsed))
                return;
            BEAST_EXPECT(parsed->list_size() <= maxManifestEntries);
            for (auto const& entry : parsed->list())
            {
                auto m = deserializeManifest(entry.stobject());
                if (BEAST_EXPECT(m))
                    BEAST_EXPECT(offered.insert(m->masterKey).second);
            }
            wire.insert(wire.end(), bytes.begin(), bytes.end());
        }
        BEAST_EXPECT(offered == keys);
        auto const remote =
            beast::IP::Endpoint(beast::IP::Address::from_string("172.1.1.240"));
        auto slot = inbound.peerFinder().new_outbound_slot(remote);
        if (!BEAST_EXPECT(slot))
            return;
        BEAST_EXPECT(inbound.peerFinder().onConnected(
            slot,
            beast::IP::Endpoint(
                beast::IP::Address::from_string("172.1.1.239"))));
        auto const nodeKey = randomKeyPair(KeyType::ed25519).first;
        BEAST_EXPECT(
            inbound.peerFinder().activate(slot, nodeKey, false) ==
            PeerFinder::Result::success);
        auto stream = std::make_unique<stream_type>(
            socket_type(receiver.app().getIOService()), *context_);
        stream->next_layer().socket().open(boost::asio::ip::tcp::v4());
        PeerTest::init();
        PeerTest::sid_ = 1;
        auto peer = std::make_shared<PeerTest>(
            receiver.app(),
            std::move(stream),
            boost::asio::buffer(wire),
            std::move(slot),
            inbound.resourceManager().newInboundEndpoint(remote),
            nodeKey,
            protocolVersion_,
            inbound);
        inbound.add_active(peer);
        BEAST_EXPECT(peer->waitSent(1));
        receiver.app().getJobQueue().rendezvous();
        for (auto const& key : keys)
            BEAST_EXPECT(target.getRawManifest(key));
        BEAST_EXPECT(target.revoked(own.first));
        BEAST_EXPECT(peer->waitCharges(3));
        peer->stop();
    }

    void
    run() override
    {
        testManifestCapacityRelay();
        testManifestIngress();
        testListedGossip();
        testManifestBatches();
        bool log = false;
        std::set<Peer::id_t> skip = {0, 1, 2, 3, 4};
        testConfig(log);
        // relay to all peers, no hash queue
        testRelay("feature disabled", false, 10, 0, 10, 25, 10, 0);
        // relay to nPeers - skip (10-5=5)
        testRelay("feature disabled & skip", false, 10, 0, 10, 25, 5, 0, skip);
        // relay to all peers because min is greater than nPeers
        testRelay("relay all 1", true, 10, 0, 20, 25, 10, 0);
        // relay to all peers because min + disabled is greater thant nPeers
        testRelay("relay all 2", true, 20, 15, 10, 25, 20, 0);
        // relay to minPeers + 25% of nPeers-minPeers (20+0.25*(60-20)=30),
        // queue the rest (30)
        testRelay("relay & queue", true, 60, 0, 20, 25, 30, 30);
        // relay to minPeers + 25% of (nPeers - nPeers) - skip
        // (20+0.25*(60-20)-5=25), queue the rest, skip counts towards relayed
        // (60-25-5=30)
        testRelay("skip", true, 60, 0, 20, 25, 25, 30, skip);
        // relay to minPeers + disabled + 25% of (nPeers - minPeers - disalbed)
        // (20+10+0.25*(70-20-10)=40), queue the rest (30)
        testRelay("disabled", true, 70, 10, 20, 25, 40, 30);
        // relay to minPeers + disabled-not-in-skip + 25% of (nPeers - minPeers
        // - disabled) (20+5+0.25*(70-20-10)=35), queue the rest, skip counts
        // towards relayed (70-35-5=30))
        testRelay("disabled & skip", true, 70, 10, 20, 25, 35, 30, skip);
        // relay to minPeers + disabled + 25% of (nPeers - minPeers - disabled)
        // - skip (10+5+0.25*(15-10-5)-10=5), queue the rest, skip counts
        // towards relayed (15-5-10=0)
        skip = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        testRelay("disabled & skip, no queue", true, 15, 5, 10, 25, 5, 0, skip);
        // relay to minPeers + disabled + 25% of (nPeers - minPeers - disabled)
        // - skip (10+2+0.25*(20-10-2)-14=0), queue the rest, skip counts
        // towards relayed (20-14=6)
        skip = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
        testRelay("disabled & skip, no relay", true, 20, 2, 10, 25, 0, 6, skip);
    }
};

BEAST_DEFINE_TESTSUITE(tx_reduce_relay, ripple_data, ripple);
}  // namespace test
}  // namespace ripple
