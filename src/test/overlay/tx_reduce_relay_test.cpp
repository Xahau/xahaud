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
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/overlay/detail/OverlayImpl.h>
#include <xrpld/overlay/detail/PeerImp.h>
#include <xrpld/peerfinder/detail/SlotImp.h>
#include <xrpl/basics/make_SSLContext.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/Sign.h>
#include <condition_variable>
#include <limits>
#include <mutex>

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

        void
        run() override
        {
        }
        void
        send(std::shared_ptr<Message> const& message) override
        {
            auto const& buffer =
                message->getBuffer(compression::Compressed::Off);
            auto const type = (static_cast<int>(buffer[4]) << 8) |
                static_cast<int>(buffer[5]);
            {
                std::lock_guard lock(sentMutex_);
                sentTypes_.push_back(type);
                if (type == protocol::mtMANIFESTS)
                {
                    protocol::TMManifests manifests;
                    if (manifests.ParseFromArray(
                            buffer.data() + compression::headerBytes,
                            static_cast<int>(
                                buffer.size() - compression::headerBytes)))
                    {
                        for (auto const& manifest : manifests.list())
                            sentManifestPayloads_.push_back(
                                manifest.stobject());
                    }
                }
                sendTx_++;
            }
            sentCv_.notify_all();
        }

        template <class MessageType>
        bool
        receive(MessageType const& message, protocol::MessageType type)
        {
            Message wire(message, type);
            auto const& buffer = wire.getBuffer(compression::Compressed::Off);
            std::size_t hint = 0;
            auto const [consumed, ec] =
                invokeProtocolMessage(boost::asio::buffer(buffer), *this, hint);
            return !ec && consumed == buffer.size();
        }

        void
        addTxQueue(const uint256& hash) override
        {
            queueTx_++;
        }
        static void
        init()
        {
            std::lock_guard lock(sentMutex_);
            queueTx_ = 0;
            sendTx_ = 0;
            sid_ = 0;
            sentTypes_.clear();
            sentManifestPayloads_.clear();
        }
        static bool
        waitForMessages(std::size_t count)
        {
            using namespace std::chrono_literals;
            std::unique_lock lock(sentMutex_);
            return sentCv_.wait_for(
                lock, 5s, [count] { return sentTypes_.size() >= count; });
        }
        static bool
        waitForMessageQuiescence()
        {
            using namespace std::chrono_literals;
            auto const deadline = std::chrono::steady_clock::now() + 5s;
            std::unique_lock lock(sentMutex_);
            for (;;)
            {
                auto const count = sentTypes_.size();
                auto const quietUntil =
                    std::min(deadline, std::chrono::steady_clock::now() + 50ms);
                if (!sentCv_.wait_until(lock, quietUntil, [count] {
                        return sentTypes_.size() != count;
                    }))
                    return true;
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
            }
        }
        static std::vector<int>
        sentTypes()
        {
            std::lock_guard lock(sentMutex_);
            return sentTypes_;
        }
        static std::vector<std::string>
        sentManifestPayloads()
        {
            std::lock_guard lock(sentMutex_);
            return sentManifestPayloads_;
        }
        inline static std::size_t sid_ = 0;
        inline static std::uint16_t queueTx_ = 0;
        inline static std::uint16_t sendTx_ = 0;
        inline static std::mutex sentMutex_;
        inline static std::condition_variable sentCv_;
        inline static std::vector<int> sentTypes_;
        inline static std::vector<std::string> sentManifestPayloads_;
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
    testManifestBeforeValidation()
    {
        testcase("manifest precedes validation per connection");

        jtx::Env env(*this);
        std::vector<std::shared_ptr<PeerTest>> peers;
        std::uint16_t disabled = 0;
        PeerTest::init();
        lid_ = 0;
        rid_ = 0;
        addPeer(env, peers, disabled);

        auto const masterSecret = randomSecretKey();
        auto const masterKey = derivePublicKey(KeyType::ed25519, masterSecret);
        BEAST_EXPECT(env.app().validators().load(
            std::nullopt, {toBase58(TokenType::NodePublic, masterKey)}, {}));
        BEAST_EXPECT(env.app().validators().listed(masterKey));

        auto makeManifest = [&](SecretKey const& signingSecret, int sequence) {
            auto const signingKey =
                derivePublicKey(KeyType::secp256k1, signingSecret);
            STObject st(sfGeneric);
            st[sfSequence] = sequence;
            st[sfPublicKey] = masterKey;
            st[sfSigningPubKey] = signingKey;
            sign(st, HashPrefix::manifest, KeyType::secp256k1, signingSecret);
            sign(
                st,
                HashPrefix::manifest,
                KeyType::ed25519,
                masterSecret,
                sfMasterSignature);
            Serializer serializer;
            st.add(serializer);
            auto manifest = deserializeManifest(std::string(
                static_cast<char const*>(serializer.data()),
                serializer.size()));
            if (!manifest)
                Throw<std::runtime_error>("could not create test manifest");
            return std::move(*manifest);
        };

        auto const signingSecret0 = randomSecretKey();
        auto const signingKey0 =
            derivePublicKey(KeyType::secp256k1, signingSecret0);
        BEAST_EXPECT(
            env.app().validatorManifests().applyManifest(makeManifest(
                signingSecret0, 0)) == ManifestDisposition::accepted);

        protocol::TMValidation validation;
        validation.set_validation("validation");
        env.app().overlay().broadcast(validation, signingKey0);
        BEAST_EXPECT(PeerTest::waitForMessages(2));
        BEAST_EXPECT(
            (PeerTest::sentTypes() ==
             std::vector<int>{protocol::mtMANIFESTS, protocol::mtVALIDATION}));

        // The sender cannot infer the receiver's local validator policy, so
        // even a locally listed prerequisite accompanies every validation.
        env.app().overlay().broadcast(validation, signingKey0);
        BEAST_EXPECT(PeerTest::waitForMessages(4));
        BEAST_EXPECT(
            (PeerTest::sentTypes() ==
             std::vector<int>{
                 protocol::mtMANIFESTS,
                 protocol::mtVALIDATION,
                 protocol::mtMANIFESTS,
                 protocol::mtVALIDATION}));

        // A new signing key/sequence is ordered before its first validation.
        auto const signingSecret1 = randomSecretKey();
        auto const signingKey1 =
            derivePublicKey(KeyType::secp256k1, signingSecret1);
        auto manifest1 = makeManifest(signingSecret1, 1);
        BEAST_EXPECT(
            env.app().validatorManifests().applyManifest(
                std::move(manifest1)) == ManifestDisposition::accepted);
        auto const corrected =
            env.app().validatorManifests().getManifestSnapshot(masterKey);
        BEAST_EXPECT(
            corrected && corrected->sequence == 1 &&
            corrected->signingKey == signingKey1);
        env.app().overlay().broadcast(validation, signingKey1);
        BEAST_EXPECT(PeerTest::waitForMessages(6));
        BEAST_EXPECT(
            (PeerTest::sentTypes() ==
             std::vector<int>{
                 protocol::mtMANIFESTS,
                 protocol::mtVALIDATION,
                 protocol::mtMANIFESTS,
                 protocol::mtVALIDATION,
                 protocol::mtMANIFESTS,
                 protocol::mtVALIDATION}));

        // Unlisted prerequisites obey the same stateless pairing rule.
        PeerTest::init();
        auto const ephemeralMasterSecret = randomSecretKey();
        auto const ephemeralMasterKey =
            derivePublicKey(KeyType::ed25519, ephemeralMasterSecret);
        auto const ephemeralSigningSecret = randomSecretKey();
        auto const ephemeralSigningKey =
            derivePublicKey(KeyType::secp256k1, ephemeralSigningSecret);
        STObject ephemeralObject(sfGeneric);
        ephemeralObject[sfSequence] = 0;
        ephemeralObject[sfPublicKey] = ephemeralMasterKey;
        ephemeralObject[sfSigningPubKey] = ephemeralSigningKey;
        sign(
            ephemeralObject,
            HashPrefix::manifest,
            KeyType::secp256k1,
            ephemeralSigningSecret);
        sign(
            ephemeralObject,
            HashPrefix::manifest,
            KeyType::ed25519,
            ephemeralMasterSecret,
            sfMasterSignature);
        Serializer ephemeralSerializer;
        ephemeralObject.add(ephemeralSerializer);
        auto ephemeralManifest = deserializeManifest(std::string(
            static_cast<char const*>(ephemeralSerializer.data()),
            ephemeralSerializer.size()));
        BEAST_EXPECT(ephemeralManifest);
        if (!ephemeralManifest)
            return;
        BEAST_EXPECT(
            env.app().validatorManifests().applyManifest(std::move(
                *ephemeralManifest)) == ManifestDisposition::accepted);
        env.app().overlay().broadcast(validation, ephemeralSigningKey);
        env.app().overlay().broadcast(validation, ephemeralSigningKey);
        BEAST_EXPECT(PeerTest::waitForMessages(4));
        BEAST_EXPECT(
            (PeerTest::sentTypes() ==
             std::vector<int>{
                 protocol::mtMANIFESTS,
                 protocol::mtVALIDATION,
                 protocol::mtMANIFESTS,
                 protocol::mtVALIDATION}));
    }

    void
    testManifestRevocation()
    {
        testcase("manifest revocation propagation");

        jtx::Env env(*this);
        std::vector<std::shared_ptr<PeerTest>> peers;
        std::uint16_t disabled = 0;
        PeerTest::init();
        lid_ = 0;
        rid_ = 0;
        addPeer(env, peers, disabled);

        auto const masterSecret = randomSecretKey();
        auto const masterKey = derivePublicKey(KeyType::ed25519, masterSecret);
        BEAST_EXPECT(env.app().validators().load(
            std::nullopt, {toBase58(TokenType::NodePublic, masterKey)}, {}));
        BEAST_EXPECT(env.app().validators().listed(masterKey));
        auto const signingSecret = randomSecretKey();
        auto const signingKey =
            derivePublicKey(KeyType::secp256k1, signingSecret);

        auto serialize = [](STObject const& object) {
            Serializer serializer;
            object.add(serializer);
            return std::string(
                static_cast<char const*>(serializer.data()), serializer.size());
        };

        STObject manifestObject(sfGeneric);
        manifestObject[sfSequence] = 0;
        manifestObject[sfPublicKey] = masterKey;
        manifestObject[sfSigningPubKey] = signingKey;
        sign(
            manifestObject,
            HashPrefix::manifest,
            KeyType::secp256k1,
            signingSecret);
        sign(
            manifestObject,
            HashPrefix::manifest,
            KeyType::ed25519,
            masterSecret,
            sfMasterSignature);
        auto const manifestSerialized = serialize(manifestObject);
        auto manifest = deserializeManifest(manifestSerialized);
        BEAST_EXPECT(manifest);
        if (!manifest)
            return;
        BEAST_EXPECT(
            env.app().validatorManifests().applyManifest(
                std::move(*manifest)) == ManifestDisposition::accepted);

        STObject revocationObject(sfGeneric);
        revocationObject[sfSequence] =
            std::numeric_limits<std::uint32_t>::max();
        revocationObject[sfPublicKey] = masterKey;
        sign(
            revocationObject,
            HashPrefix::manifest,
            KeyType::ed25519,
            masterSecret,
            sfMasterSignature);
        auto const revocationSerialized = serialize(revocationObject);

        // Revocation has no following validation, so accepted-manifest relay
        // remains its immediate propagation path.
        auto revocations = std::make_shared<protocol::TMManifests>();
        revocations->add_list()->set_stobject(revocationSerialized);
        BEAST_EXPECT(
            peers.front()->receive(*revocations, protocol::mtMANIFESTS));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(env.app().validatorManifests().revoked(masterKey));
        BEAST_EXPECT(PeerTest::waitForMessages(1));
        BEAST_EXPECT(
            (PeerTest::sentTypes() == std::vector<int>{protocol::mtMANIFESTS}));
        BEAST_EXPECT(
            (PeerTest::sentManifestPayloads() ==
             std::vector<std::string>{revocationSerialized}));

        // A later validation under the revoked signing key must not resurrect
        // or resend the superseded manifest.
        PeerTest::init();
        protocol::TMValidation validation;
        validation.set_validation("validation");
        env.app().overlay().broadcast(validation, signingKey);
        BEAST_EXPECT(PeerTest::waitForMessages(1));
        BEAST_EXPECT((
            PeerTest::sentTypes() == std::vector<int>{protocol::mtVALIDATION}));
        BEAST_EXPECT(PeerTest::sentManifestPayloads().empty());

        // Ambient stale/newer correction replies are deliberately parked: a
        // stale singleton cannot turn manifest relay into ping-pong.
        PeerTest::init();
        auto stale = std::make_shared<protocol::TMManifests>();
        stale->add_list()->set_stobject(manifestSerialized);
        dynamic_cast<OverlayImpl&>(env.app().overlay())
            .onManifests(stale, peers.front());
        BEAST_EXPECT(PeerTest::sentTypes().empty());
        BEAST_EXPECT(PeerTest::sentManifestPayloads().empty());
        BEAST_EXPECT(env.app().validatorManifests().revoked(masterKey));
    }

    void
    testIncomingManifestCandidate()
    {
        testcase("incoming manifest candidate is connection-bounded");

        auto config = jtx::envconfig();
        config->WORKERS = 1;
        jtx::Env env(*this, std::move(config));
        std::vector<std::shared_ptr<PeerTest>> peers;
        std::uint16_t disabled = 0;
        PeerTest::init();
        lid_ = 0;
        rid_ = 0;
        addPeer(env, peers, disabled);
        auto const peer = peers.front();

        auto serialize = [](STObject const& object) {
            Serializer serializer;
            object.add(serializer);
            return std::string(
                static_cast<char const*>(serializer.data()), serializer.size());
        };

        auto makeManifest = [&](SecretKey const& masterSecret,
                                PublicKey const& masterKey,
                                SecretKey const& signingSecret,
                                std::uint32_t sequence,
                                SecretKey const* signatureMaster = nullptr) {
            auto const signingKey =
                derivePublicKey(KeyType::secp256k1, signingSecret);
            STObject object(sfGeneric);
            object[sfSequence] = sequence;
            object[sfPublicKey] = masterKey;
            object[sfSigningPubKey] = signingKey;
            sign(
                object,
                HashPrefix::manifest,
                KeyType::secp256k1,
                signingSecret);
            sign(
                object,
                HashPrefix::manifest,
                KeyType::ed25519,
                signatureMaster ? *signatureMaster : masterSecret,
                sfMasterSignature);
            return serialize(object);
        };

        auto makeValidationAt = [&](PublicKey const& masterKey,
                                    PublicKey const& signingKey,
                                    SecretKey const& signingSecret,
                                    NetClock::time_point signTime,
                                    uint256 ledgerHash) {
            STValidation validation(
                signTime,
                signingKey,
                signingSecret,
                calcNodeID(masterKey),
                [ledgerHash](STValidation& value) {
                    value.setFieldH256(sfLedgerHash, ledgerHash);
                    value.setFieldU32(sfLedgerSequence, 1);
                });
            auto const serialized = validation.getSerialized();
            protocol::TMValidation message;
            message.set_validation(serialized.data(), serialized.size());
            return message;
        };

        auto makeValidation = [&](PublicKey const& masterKey,
                                  PublicKey const& signingKey,
                                  SecretKey const& signingSecret) {
            return makeValidationAt(
                masterKey,
                signingKey,
                signingSecret,
                env.app().timeKeeper().closeTime(),
                uint256{1});
        };

        auto receiveManifest = [&](std::string const& serialized) {
            protocol::TMManifests message;
            message.add_list()->set_stobject(serialized);
            return peer->receive(message, protocol::mtMANIFESTS);
        };

        auto listMaster = [&](PublicKey const& masterKey) {
            BEAST_EXPECT(env.app().validators().load(
                std::nullopt,
                {toBase58(TokenType::NodePublic, masterKey)},
                {}));
            BEAST_EXPECT(env.app().validators().listed(masterKey));
        };

        struct JobGate
        {
            std::mutex mutex;
            std::condition_variable cv;
            bool started = false;
            bool released = false;
        };
        auto holdJobQueue = [&]() {
            auto gate = std::make_shared<JobGate>();
            BEAST_EXPECT(env.app().getJobQueue().addJob(
                jtCLIENT, "hold manifest pair verification", [gate]() {
                    std::unique_lock lock(gate->mutex);
                    gate->started = true;
                    gate->cv.notify_all();
                    gate->cv.wait(lock, [gate] { return gate->released; });
                }));
            {
                std::unique_lock lock(gate->mutex);
                BEAST_EXPECT(
                    gate->cv.wait_for(lock, std::chrono::seconds{5}, [gate] {
                        return gate->started;
                    }));
            }
            return gate;
        };
        auto releaseJobQueue = [&](std::shared_ptr<JobGate> const& gate) {
            {
                std::lock_guard lock(gate->mutex);
                gate->released = true;
            }
            gate->cv.notify_all();
            env.app().getJobQueue().rendezvous();
        };
        auto validationJobCount = [&]() {
            return env.app().getJobQueue().getJobCountTotal(jtVALIDATION_t) +
                env.app().getJobQueue().getJobCountTotal(jtVALIDATION_ut);
        };

        // A claimed candidate owns exactly one crypto job. While that job is
        // held, one later candidate may occupy the now-free connection slot;
        // its validation is deliberately dropped and heals when the sender
        // repeats the pair after the first job completes.
        testcase("manifest pair verification is one-in-flight");
        auto const burstMasterSecret0 = randomSecretKey();
        auto const burstMasterKey0 =
            derivePublicKey(KeyType::ed25519, burstMasterSecret0);
        listMaster(burstMasterKey0);
        auto const burstSigningSecret0 = randomSecretKey();
        auto const burstSigningKey0 =
            derivePublicKey(KeyType::secp256k1, burstSigningSecret0);
        auto const burstManifest0 = makeManifest(
            burstMasterSecret0, burstMasterKey0, burstSigningSecret0, 0);
        auto const burstValidation0 = makeValidation(
            burstMasterKey0, burstSigningKey0, burstSigningSecret0);

        auto const burstMasterSecret1 = randomSecretKey();
        auto const burstMasterKey1 =
            derivePublicKey(KeyType::ed25519, burstMasterSecret1);
        listMaster(burstMasterKey1);
        auto const burstSigningSecret1 = randomSecretKey();
        auto const burstSigningKey1 =
            derivePublicKey(KeyType::secp256k1, burstSigningSecret1);
        auto const burstManifest1 = makeManifest(
            burstMasterSecret1, burstMasterKey1, burstSigningSecret1, 0);
        auto const burstValidation1 = makeValidation(
            burstMasterKey1, burstSigningKey1, burstSigningSecret1);

        auto const gate0 = holdJobQueue();
        auto const jobsBeforeBurst = validationJobCount();
        BEAST_EXPECT(receiveManifest(burstManifest0));
        BEAST_EXPECT(peer->receive(burstValidation0, protocol::mtVALIDATION));
        BEAST_EXPECT(receiveManifest(burstManifest1));
        BEAST_EXPECT(peer->receive(burstValidation1, protocol::mtVALIDATION));
        BEAST_EXPECT(validationJobCount() == jobsBeforeBurst + 1);
        releaseJobQueue(gate0);

        auto const burstAdmitted0 =
            env.app().validatorManifests().getManifestSnapshot(burstMasterKey0);
        BEAST_EXPECT(
            burstAdmitted0 && burstAdmitted0->signingKey == burstSigningKey0);
        BEAST_EXPECT(!env.app().validatorManifests().getManifestSnapshot(
            burstMasterKey1));

        // The second candidate stayed bounded in the waiting slot. A repeated
        // pair after completion proves that it was neither displaced nor
        // permanently wedged by the first job.
        BEAST_EXPECT(receiveManifest(burstManifest1));
        BEAST_EXPECT(peer->receive(burstValidation1, protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        auto const burstAdmitted1 =
            env.app().validatorManifests().getManifestSnapshot(burstMasterKey1);
        BEAST_EXPECT(
            burstAdmitted1 && burstAdmitted1->signingKey == burstSigningKey1);

        // One forged listed-looking candidate cannot be copied into an
        // unbounded number of distinct validation jobs. The peer is expected
        // to receive a terminal signature charge when the sole job runs, so
        // isolate this pressure control on its own connection.
        addPeer(env, peers, disabled);
        auto const burstPeer = peers.back();
        auto const forgedBurstMasterSecret = randomSecretKey();
        auto const forgedBurstMasterKey =
            derivePublicKey(KeyType::ed25519, forgedBurstMasterSecret);
        listMaster(forgedBurstMasterKey);
        auto const forgedBurstSigningSecret = randomSecretKey();
        auto const forgedBurstSigningKey =
            derivePublicKey(KeyType::secp256k1, forgedBurstSigningSecret);
        auto const wrongBurstMasterSecret = randomSecretKey();
        protocol::TMManifests forgedBurstManifest;
        forgedBurstManifest.add_list()->set_stobject(makeManifest(
            forgedBurstMasterSecret,
            forgedBurstMasterKey,
            forgedBurstSigningSecret,
            0,
            &wrongBurstMasterSecret));

        auto const gate1 = holdJobQueue();
        auto const jobsBeforeForgedBurst = validationJobCount();
        BEAST_EXPECT(
            burstPeer->receive(forgedBurstManifest, protocol::mtMANIFESTS));
        for (std::uint64_t i = 0; i < 8; ++i)
        {
            auto validation = makeValidationAt(
                forgedBurstMasterKey,
                forgedBurstSigningKey,
                forgedBurstSigningSecret,
                env.app().timeKeeper().closeTime(),
                uint256{i + 10});
            BEAST_EXPECT(
                burstPeer->receive(validation, protocol::mtVALIDATION));
        }
        BEAST_EXPECT(validationJobCount() == jobsBeforeForgedBurst + 1);
        releaseJobQueue(gate1);
        BEAST_EXPECT(!env.app().validatorManifests().getManifestSnapshot(
            forgedBurstMasterKey));

        // A normal manifest is only structurally staged on receipt. The first
        // candidate owns the connection's bounded slot until a matching
        // validation consumes it; interleaved singleton manifests cannot
        // displace it.
        auto const masterSecret0 = randomSecretKey();
        auto const masterKey0 =
            derivePublicKey(KeyType::ed25519, masterSecret0);
        listMaster(masterKey0);
        auto const signingSecret0 = randomSecretKey();
        auto const signingKey0 =
            derivePublicKey(KeyType::secp256k1, signingSecret0);
        auto const serialized0 =
            makeManifest(masterSecret0, masterKey0, signingSecret0, 0);
        BEAST_EXPECT(receiveManifest(serialized0));
        BEAST_EXPECT(
            !env.app().validatorManifests().getManifestSnapshot(masterKey0));

        auto const displacedMasterSecret = randomSecretKey();
        auto const displacedMasterKey =
            derivePublicKey(KeyType::ed25519, displacedMasterSecret);
        listMaster(displacedMasterKey);
        auto const displacedSigningSecret = randomSecretKey();
        auto const displacedSigningKey =
            derivePublicKey(KeyType::secp256k1, displacedSigningSecret);
        BEAST_EXPECT(receiveManifest(makeManifest(
            displacedMasterSecret,
            displacedMasterKey,
            displacedSigningSecret,
            0)));

        auto const validation0 =
            makeValidation(masterKey0, signingKey0, signingSecret0);
        BEAST_EXPECT(peer->receive(validation0, protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        auto const admitted0 =
            env.app().validatorManifests().getManifestSnapshot(masterKey0);
        BEAST_EXPECT(admitted0 && admitted0->signingKey == signingKey0);
        BEAST_EXPECT(!env.app().validatorManifests().getManifestSnapshot(
            displacedMasterKey));

        // Consuming the first candidate releases the slot for later traffic.
        BEAST_EXPECT(receiveManifest(makeManifest(
            displacedMasterSecret,
            displacedMasterKey,
            displacedSigningSecret,
            0)));
        BEAST_EXPECT(peer->receive(
            makeValidation(
                displacedMasterKey,
                displacedSigningKey,
                displacedSigningSecret),
            protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        auto const admittedDisplaced =
            env.app().validatorManifests().getManifestSnapshot(
                displacedMasterKey);
        BEAST_EXPECT(
            admittedDisplaced &&
            admittedDisplaced->signingKey == displacedSigningKey);

        // A lost validation must not wedge a sequence bump on this
        // connection. A newer manifest for the same master supersedes the
        // unconsumed candidate in the same bounded slot.
        auto const advancingMasterSecret = randomSecretKey();
        auto const advancingMasterKey =
            derivePublicKey(KeyType::ed25519, advancingMasterSecret);
        listMaster(advancingMasterKey);
        auto const advancingSigningSecret8 = randomSecretKey();
        BEAST_EXPECT(receiveManifest(makeManifest(
            advancingMasterSecret,
            advancingMasterKey,
            advancingSigningSecret8,
            8)));
        auto const advancingSigningSecret9 = randomSecretKey();
        auto const advancingSigningKey9 =
            derivePublicKey(KeyType::secp256k1, advancingSigningSecret9);
        BEAST_EXPECT(receiveManifest(makeManifest(
            advancingMasterSecret,
            advancingMasterKey,
            advancingSigningSecret9,
            9)));
        BEAST_EXPECT(peer->receive(
            makeValidation(
                advancingMasterKey,
                advancingSigningKey9,
                advancingSigningSecret9),
            protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        auto const admittedAdvancing =
            env.app().validatorManifests().getManifestSnapshot(
                advancingMasterKey);
        BEAST_EXPECT(
            admittedAdvancing && admittedAdvancing->sequence == 9 &&
            admittedAdvancing->signingKey == advancingSigningKey9);

        // Old peers may send a one-item startup cache before status traffic.
        // Unrelated frames neither admit nor discard the bounded candidate.
        auto const masterSecret1 = randomSecretKey();
        auto const masterKey1 =
            derivePublicKey(KeyType::ed25519, masterSecret1);
        listMaster(masterKey1);
        auto const signingSecret1 = randomSecretKey();
        auto const signingKey1 =
            derivePublicKey(KeyType::secp256k1, signingSecret1);
        BEAST_EXPECT(receiveManifest(
            makeManifest(masterSecret1, masterKey1, signingSecret1, 0)));
        protocol::TMPing ping;
        ping.set_type(protocol::TMPing::ptPING);
        ping.set_seq(1);
        BEAST_EXPECT(peer->receive(ping, protocol::mtPING));

        // A matching but non-current validation does not consume the
        // candidate; the subsequent current validation still proves it.
        BEAST_EXPECT(peer->receive(
            makeValidationAt(
                masterKey1,
                signingKey1,
                signingSecret1,
                NetClock::time_point{},
                uint256{1}),
            protocol::mtVALIDATION));
        BEAST_EXPECT(peer->receive(
            makeValidation(masterKey1, signingKey1, signingSecret1),
            protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        auto const admitted1 =
            env.app().validatorManifests().getManifestSnapshot(masterKey1);
        BEAST_EXPECT(admitted1 && admitted1->signingKey == signingKey1);

        // A validation for another signing key does not consume or apply the
        // candidate. A later matching validation may still prove it.
        auto const masterSecret2 = randomSecretKey();
        auto const masterKey2 =
            derivePublicKey(KeyType::ed25519, masterSecret2);
        listMaster(masterKey2);
        auto const signingSecret2 = randomSecretKey();
        BEAST_EXPECT(receiveManifest(
            makeManifest(masterSecret2, masterKey2, signingSecret2, 0)));
        auto const otherSecret = randomSecretKey();
        auto const otherKey = derivePublicKey(KeyType::secp256k1, otherSecret);
        BEAST_EXPECT(peer->receive(
            makeValidation(otherKey, otherKey, otherSecret),
            protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(
            !env.app().validatorManifests().getManifestSnapshot(masterKey2));
        auto const signingKey2 =
            derivePublicKey(KeyType::secp256k1, signingSecret2);
        BEAST_EXPECT(peer->receive(
            makeValidation(masterKey2, signingKey2, signingSecret2),
            protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        auto const admitted2 =
            env.app().validatorManifests().getManifestSnapshot(masterKey2);
        BEAST_EXPECT(admitted2 && admitted2->signingKey == signingKey2);

        // A fully valid pair for an unlisted identity remains ephemeral. It
        // cannot create a permanent ManifestCache entry merely by proving
        // control of keys the peer minted itself.
        auto const unlistedMasterSecret = randomSecretKey();
        auto const unlistedMasterKey =
            derivePublicKey(KeyType::ed25519, unlistedMasterSecret);
        auto const unlistedSigningSecret = randomSecretKey();
        auto const unlistedSigningKey =
            derivePublicKey(KeyType::secp256k1, unlistedSigningSecret);
        auto const unlistedSerialized = makeManifest(
            unlistedMasterSecret, unlistedMasterKey, unlistedSigningSecret, 0);
        BEAST_EXPECT(receiveManifest(unlistedSerialized));
        BEAST_EXPECT(peer->receive(
            makeValidation(
                unlistedMasterKey, unlistedSigningKey, unlistedSigningSecret),
            protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(!env.app().validatorManifests().getManifestSnapshot(
            unlistedMasterKey));

        // A legacy/pre-existing cache row is observation, not admission
        // authority. A valid advancing pair remains ephemeral until local
        // policy lists the master.
        auto const residueMasterSecret = randomSecretKey();
        auto const residueMasterKey =
            derivePublicKey(KeyType::ed25519, residueMasterSecret);
        auto const residueSigningSecret0 = randomSecretKey();
        auto const residueSigningKey0 =
            derivePublicKey(KeyType::secp256k1, residueSigningSecret0);
        auto residue0 = deserializeManifest(makeManifest(
            residueMasterSecret, residueMasterKey, residueSigningSecret0, 0));
        BEAST_EXPECT(residue0);
        if (!residue0)
            return;
        BEAST_EXPECT(
            env.app().validatorManifests().applyManifest(
                std::move(*residue0)) == ManifestDisposition::accepted);

        auto const residueSigningSecret1 = randomSecretKey();
        auto const residueSigningKey1 =
            derivePublicKey(KeyType::secp256k1, residueSigningSecret1);
        BEAST_EXPECT(receiveManifest(makeManifest(
            residueMasterSecret, residueMasterKey, residueSigningSecret1, 1)));
        BEAST_EXPECT(peer->receive(
            makeValidation(
                residueMasterKey, residueSigningKey1, residueSigningSecret1),
            protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        auto const residueCurrent =
            env.app().validatorManifests().getManifestSnapshot(
                residueMasterKey);
        BEAST_EXPECT(
            residueCurrent && residueCurrent->sequence == 0 &&
            residueCurrent->signingKey == residueSigningKey0);

        // The legacy batch compatibility lane obeys the same durable-cache
        // admission boundary.
        protocol::TMManifests unlistedLegacyBatch;
        unlistedLegacyBatch.add_list()->set_stobject(unlistedSerialized);
        unlistedLegacyBatch.add_list()->set_stobject(unlistedSerialized);
        BEAST_EXPECT(peer->receive(unlistedLegacyBatch, protocol::mtMANIFESTS));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(!env.app().validatorManifests().getManifestSnapshot(
            unlistedMasterKey));

        // Oversized legacy batches are charged and refused rather than
        // processed after the charge.
        auto const masterSecret5 = randomSecretKey();
        auto const masterKey5 =
            derivePublicKey(KeyType::ed25519, masterSecret5);
        auto const signingSecret5 = randomSecretKey();
        auto const serialized5 =
            makeManifest(masterSecret5, masterKey5, signingSecret5, 0);
        protocol::TMManifests oversized;
        for (int i = 0; i < 101; ++i)
            oversized.add_list()->set_stobject(serialized5);
        BEAST_EXPECT(peer->receive(oversized, protocol::mtMANIFESTS));
        BEAST_EXPECT(
            !env.app().validatorManifests().getManifestSnapshot(masterKey5));

        protocol::TMManifests oversizedObject;
        oversizedObject.add_list()->set_stobject(std::string(4097, 'x'));
        BEAST_EXPECT(peer->receive(oversizedObject, protocol::mtMANIFESTS));

        // A naked validation whose signer has no trusted or retained master
        // mapping is not useful, and must not front-run the real validation
        // hash. A later verified pair remains processable and relays in wire
        // order to another connection.
        testcase("naked validation cannot front-run verified pair");
        addPeer(env, peers, disabled);
        auto const repairMasterSecret = randomSecretKey();
        auto const repairMasterKey =
            derivePublicKey(KeyType::ed25519, repairMasterSecret);
        auto const repairSigningSecret = randomSecretKey();
        auto const repairSigningKey =
            derivePublicKey(KeyType::secp256k1, repairSigningSecret);
        auto const repairManifest = makeManifest(
            repairMasterSecret, repairMasterKey, repairSigningSecret, 0);
        auto const repairValidation = makeValidation(
            repairMasterKey, repairSigningKey, repairSigningSecret);
        BEAST_EXPECT(PeerTest::waitForMessageQuiescence());
        auto const messagesBeforeRepair = PeerTest::sentTypes().size();

        BEAST_EXPECT(peer->receive(repairValidation, protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(PeerTest::sentTypes().size() == messagesBeforeRepair);

        BEAST_EXPECT(receiveManifest(repairManifest));
        BEAST_EXPECT(peer->receive(repairValidation, protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(PeerTest::waitForMessages(messagesBeforeRepair + 2));
        auto const repairedTypes = PeerTest::sentTypes();
        if (repairedTypes.size() >= 2)
        {
            BEAST_EXPECT(
                repairedTypes[repairedTypes.size() - 2] ==
                protocol::mtMANIFESTS);
            BEAST_EXPECT(repairedTypes.back() == protocol::mtVALIDATION);
        }
        BEAST_EXPECT(!env.app().validatorManifests().getManifestSnapshot(
            repairMasterKey));

        // A signing key retained for one master cannot be endorsed for a
        // second master through the ephemeral pair path. Rejection must also
        // happen before the pair claims the validation's ordinary suppression
        // identity, so later valid traffic is not poisoned.
        testcase("manifest key-role collision is terminal");
        auto const collisionSigningSecret = randomSecretKey();
        auto const collisionSigningKey =
            derivePublicKey(KeyType::secp256k1, collisionSigningSecret);
        auto const collisionMasterSecret0 = randomSecretKey();
        auto const collisionMasterKey0 =
            derivePublicKey(KeyType::ed25519, collisionMasterSecret0);
        auto collisionManifest0 = deserializeManifest(makeManifest(
            collisionMasterSecret0,
            collisionMasterKey0,
            collisionSigningSecret,
            0));
        BEAST_EXPECT(collisionManifest0);
        if (!collisionManifest0)
            return;
        BEAST_EXPECT(
            env.app().validatorManifests().applyManifest(std::move(
                *collisionManifest0)) == ManifestDisposition::accepted);

        auto const collisionMasterSecret1 = randomSecretKey();
        auto const collisionMasterKey1 =
            derivePublicKey(KeyType::ed25519, collisionMasterSecret1);
        auto const collisionManifest1 = makeManifest(
            collisionMasterSecret1,
            collisionMasterKey1,
            collisionSigningSecret,
            0);
        auto const collisionValidation1 = makeValidation(
            collisionMasterKey1, collisionSigningKey, collisionSigningSecret);
        auto const messagesBeforeCollision = PeerTest::sentTypes().size();
        BEAST_EXPECT(receiveManifest(collisionManifest1));
        BEAST_EXPECT(
            peer->receive(collisionValidation1, protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(PeerTest::sentTypes().size() == messagesBeforeCollision);
        BEAST_EXPECT(!env.app().validatorManifests().getManifestSnapshot(
            collisionMasterKey1));
        auto const collisionValidationHash1 =
            sha512Half(makeSlice(collisionValidation1.validation()));
        BEAST_EXPECT(
            env.app()
                .getHashRouter()
                .addSuppressionPeerWithStatus(collisionValidationHash1, 65001)
                .first);

        // The key-role refusal completed the sole in-flight job. A fresh
        // candidate on the same connection can therefore claim the slot and
        // complete normally.
        auto const afterFailureMasterSecret = randomSecretKey();
        auto const afterFailureMasterKey =
            derivePublicKey(KeyType::ed25519, afterFailureMasterSecret);
        listMaster(afterFailureMasterKey);
        auto const afterFailureSigningSecret = randomSecretKey();
        auto const afterFailureSigningKey =
            derivePublicKey(KeyType::secp256k1, afterFailureSigningSecret);
        BEAST_EXPECT(receiveManifest(makeManifest(
            afterFailureMasterSecret,
            afterFailureMasterKey,
            afterFailureSigningSecret,
            0)));
        BEAST_EXPECT(peer->receive(
            makeValidation(
                afterFailureMasterKey,
                afterFailureSigningKey,
                afterFailureSigningSecret),
            protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        auto const admittedAfterFailure =
            env.app().validatorManifests().getManifestSnapshot(
                afterFailureMasterKey);
        BEAST_EXPECT(
            admittedAfterFailure &&
            admittedAfterFailure->signingKey == afterFailureSigningKey);

        // A valid manifest followed by a parseable validation with a bad
        // signature must not gain cache admission merely because the signing
        // key claim matches. Keep this terminal fee case last: the production
        // resource policy intentionally makes the synthetic peer unusable
        // after feeInvalidSignature.
        auto const invalidMasterSecret = randomSecretKey();
        auto const invalidMasterKey =
            derivePublicKey(KeyType::ed25519, invalidMasterSecret);
        listMaster(invalidMasterKey);
        auto const invalidSigningSecret = randomSecretKey();
        auto const invalidSigningKey =
            derivePublicKey(KeyType::secp256k1, invalidSigningSecret);
        BEAST_EXPECT(receiveManifest(makeManifest(
            invalidMasterSecret, invalidMasterKey, invalidSigningSecret, 0)));
        auto invalidValidation = makeValidation(
            invalidMasterKey, invalidSigningKey, invalidSigningSecret);
        auto* serializedValidation = invalidValidation.mutable_validation();
        if (BEAST_EXPECT(!serializedValidation->empty()))
            serializedValidation->back() ^= 0x01;
        BEAST_EXPECT(peer->receive(invalidValidation, protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(!env.app().validatorManifests().getManifestSnapshot(
            invalidMasterKey));

        // Matching does not imply trust: a structurally valid manifest with a
        // forged master signature is rejected. Give this second terminal-fee
        // case its own peer so it cannot inherit the preceding disconnect.
        addPeer(env, peers, disabled);
        auto const forgedPeer = peers.back();
        auto const forgedMasterSecret = randomSecretKey();
        auto const forgedMasterKey =
            derivePublicKey(KeyType::ed25519, forgedMasterSecret);
        listMaster(forgedMasterKey);
        auto const forgedSigningSecret = randomSecretKey();
        auto const forgedSigningKey =
            derivePublicKey(KeyType::secp256k1, forgedSigningSecret);
        auto const wrongMasterSecret = randomSecretKey();
        protocol::TMManifests forgedManifest;
        forgedManifest.add_list()->set_stobject(makeManifest(
            forgedMasterSecret,
            forgedMasterKey,
            forgedSigningSecret,
            0,
            &wrongMasterSecret));
        BEAST_EXPECT(
            forgedPeer->receive(forgedManifest, protocol::mtMANIFESTS));
        auto const forgedValidation = makeValidation(
            forgedMasterKey, forgedSigningKey, forgedSigningSecret);
        BEAST_EXPECT(
            forgedPeer->receive(forgedValidation, protocol::mtVALIDATION));
        env.app().getJobQueue().rendezvous();
        BEAST_EXPECT(!env.app().validatorManifests().getManifestSnapshot(
            forgedMasterKey));
        auto const forgedValidationHash =
            sha512Half(makeSlice(forgedValidation.validation()));
        BEAST_EXPECT(
            env.app()
                .getHashRouter()
                .addSuppressionPeerWithStatus(forgedValidationHash, 65000)
                .first);
    }

    void
    run() override
    {
        bool log = false;
        std::set<Peer::id_t> skip = {0, 1, 2, 3, 4};
        testConfig(log);
        testManifestBeforeValidation();
        testIncomingManifestCandidate();
        testManifestRevocation();
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
