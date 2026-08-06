//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright 2026 Xahau Ledger Foundation.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/jtx.h>
#include <test/jtx/Env.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/overlay/detail/OverlayImpl.h>
#include <xrpld/overlay/detail/PeerImp.h>
#include <xrpld/peerfinder/detail/SlotImp.h>
#include <xrpl/basics/make_SSLContext.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/STExchange.h>
#include <xrpl/protocol/Sign.h>

namespace ripple {
namespace test {

class manifest_relay_test : public beast::unit_test::suite
{
    using socket_type = boost::asio::ip::tcp::socket;
    using middle_type = boost::beast::tcp_stream;
    using stream_type = boost::beast::ssl_stream<middle_type>;

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
            std::unique_ptr<manifest_relay_test::stream_type>&& stream,
            OverlayImpl& overlay)
            : PeerImp(
                  app,
                  nextId_++,
                  slot,
                  std::move(request),
                  publicKey,
                  protocol,
                  consumer,
                  std::move(stream),
                  overlay)
        {
        }

        void
        run() override
        {
        }

        void
        send(std::shared_ptr<Message> const& message) override
        {
            sent_.push_back(message);
        }

        std::vector<std::shared_ptr<Message>> sent_;
        inline static Peer::id_t nextId_ = 1;
    };

    std::shared_ptr<boost::asio::ssl::context> context_{make_SSLContext("")};
    std::uint16_t endpoint_{1};

    struct ManifestData
    {
        std::string serialized;
        PublicKey masterKey;
        uint256 hash;
    };

    ManifestData
    makeManifest(bool const large = false)
    {
        auto const [masterPublic, masterSecret] =
            randomKeyPair(KeyType::ed25519);
        auto const [signingPublic, signingSecret] =
            randomKeyPair(KeyType::secp256k1);

        STObject st{sfGeneric};
        st[sfSequence] = 0;
        st[sfPublicKey] = masterPublic;
        st[sfSigningPubKey] = signingPublic;
        if (large)
        {
            st[sfDomain] =
                makeSlice(std::string(63, 'a') + "." + std::string(63, 'b'));
        }
        sign(st, HashPrefix::manifest, KeyType::secp256k1, signingSecret);
        sign(
            st,
            HashPrefix::manifest,
            KeyType::ed25519,
            masterSecret,
            sfMasterSignature);

        Serializer serialized;
        st.add(serialized);
        std::string const bytes{
            static_cast<char const*>(serialized.data()), serialized.size()};
        auto manifest = deserializeManifest(bytes);
        BEAST_EXPECT(manifest.has_value());
        return {bytes, masterPublic, manifest ? manifest->hash() : uint256{}};
    }

    std::shared_ptr<PeerTest>
    addPeer(jtx::Env& env)
    {
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        boost::beast::http::request<boost::beast::http::dynamic_body> request;
        auto stream = std::make_unique<stream_type>(
            socket_type(env.app().getIOService()), *context_);

        auto const octet = endpoint_++;
        beast::IP::Endpoint const local{beast::IP::Address::from_string(
            "172.1.1." + std::to_string(octet))};
        beast::IP::Endpoint const remote{beast::IP::Address::from_string(
            "172.1.2." + std::to_string(octet))};
        auto const peerPublic = std::get<0>(randomKeyPair(KeyType::ed25519));
        auto consumer = overlay.resourceManager().newInboundEndpoint(remote);
        auto slot = overlay.peerFinder().new_inbound_slot(local, remote);
        auto peer = std::make_shared<PeerTest>(
            env.app(),
            slot,
            std::move(request),
            peerPublic,
            ProtocolVersion{1, 7},
            consumer,
            std::move(stream),
            overlay);
        overlay.add_active(peer);
        return peer;
    }

    static std::optional<protocol::TMManifests>
    unpack(std::shared_ptr<Message> const& message)
    {
        if (!message)
            return std::nullopt;

        auto const& buffer = message->getBuffer(compression::Compressed::Off);
        if (buffer.size() <= compression::headerBytes)
            return std::nullopt;

        protocol::TMManifests manifests;
        if (!manifests.ParseFromArray(
                buffer.data() + compression::headerBytes,
                static_cast<int>(buffer.size() - compression::headerBytes)))
            return std::nullopt;
        return manifests;
    }

    void
    expectBundle(std::shared_ptr<PeerTest> const& peer, std::size_t const count)
    {
        BEAST_EXPECT(peer->sent_.size() == 1);
        if (peer->sent_.size() != 1)
            return;

        auto const relayed = unpack(peer->sent_.front());
        BEAST_EXPECT(relayed.has_value());
        if (relayed)
        {
            BEAST_EXPECT(
                static_cast<std::size_t>(relayed->list_size()) == count);
        }
    }

    void
    testBoundedReceive()
    {
        testcase("bounded receive includes trusted entries after the cap");

        auto const trusted = makeManifest();
        jtx::Env env{
            *this, jtx::envconfig([&trusted](std::unique_ptr<Config> config) {
                config->section("validators")
                    .append(toBase58(TokenType::NodePublic, trusted.masterKey));
                return config;
            })};
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const source = addPeer(env);
        auto const destination = addPeer(env);

        auto incoming = std::make_shared<protocol::TMManifests>();
        for (std::size_t i = 0; i < kMaxManifestsPerMessage + 1; ++i)
            incoming->add_list()->set_stobject(makeManifest().serialized);
        incoming->add_list()->set_stobject(trusted.serialized);

        auto const before = env.app().validatorManifests().sequence();
        overlay.onManifests(incoming, source);

        BEAST_EXPECT(
            env.app().validatorManifests().sequence() ==
            before + kMaxManifestsPerMessage + 1);
        BEAST_EXPECT(source->sent_.empty());
        expectBundle(destination, kMaxManifestsPerMessage + 1);

        auto const snapshot = unpack(overlay.getManifestsMessage());
        BEAST_EXPECT(snapshot.has_value());
        if (snapshot)
        {
            BEAST_EXPECT(snapshot->list_size() == kMaxManifestsPerMessage + 1);
            BEAST_EXPECT(
                Message::messageSize(*snapshot) <= maximumManifestsMessageSize);
        }
    }

    void
    testRelaySkipIntersection()
    {
        testcase("relay skips only peers that have every bundled manifest");

        jtx::Env env{*this};
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const source = addPeer(env);
        auto const partial = addPeer(env);
        auto const complete = addPeer(env);
        auto const fresh = addPeer(env);
        auto const first = makeManifest();
        auto const second = makeManifest();

        auto& hashRouter = env.app().getHashRouter();
        hashRouter.addSuppressionPeer(first.hash, partial->id());
        hashRouter.addSuppressionPeer(first.hash, complete->id());
        hashRouter.addSuppressionPeer(second.hash, complete->id());

        auto incoming = std::make_shared<protocol::TMManifests>();
        incoming->add_list()->set_stobject(first.serialized);
        incoming->add_list()->set_stobject(second.serialized);
        overlay.onManifests(incoming, source);

        BEAST_EXPECT(source->sent_.empty());
        BEAST_EXPECT(complete->sent_.empty());
        expectBundle(partial, 2);
        expectBundle(fresh, 2);
    }

    void
    testSnapshotByteBudget()
    {
        testcase("snapshot prioritizes trusted entries within byte budget");

        constexpr std::size_t trustedCount = 300;
        constexpr std::size_t untrustedCount = 100;
        std::vector<ManifestData> trusted;
        std::vector<ManifestData> untrusted;
        trusted.reserve(trustedCount);
        untrusted.reserve(untrustedCount);
        for (std::size_t i = 0; i < trustedCount; ++i)
            trusted.push_back(makeManifest(true));
        for (std::size_t i = 0; i < untrustedCount; ++i)
            untrusted.push_back(makeManifest());

        jtx::Env env{
            *this, jtx::envconfig([&trusted](std::unique_ptr<Config> config) {
                auto& validators = config->section("validators");
                for (auto const& manifest : trusted)
                {
                    validators.append(
                        toBase58(TokenType::NodePublic, manifest.masterKey));
                }
                return config;
            })};

        hash_set<PublicKey> trustedMasters;
        trustedMasters.reserve(trusted.size());
        auto& cache = env.app().validatorManifests();
        for (auto const& data : trusted)
        {
            BEAST_EXPECT(env.app().validators().listed(data.masterKey));
            trustedMasters.insert(data.masterKey);
            auto manifest = deserializeManifest(data.serialized);
            BEAST_EXPECT(manifest.has_value());
            if (manifest)
            {
                BEAST_EXPECT(
                    cache.applyManifest(
                        std::move(*manifest),
                        ManifestRateLimitCapPolicy::Uncapped) ==
                    ManifestDisposition::accepted);
            }
        }
        for (auto const& data : untrusted)
        {
            BEAST_EXPECT(!env.app().validators().listed(data.masterKey));
            auto manifest = deserializeManifest(data.serialized);
            BEAST_EXPECT(manifest.has_value());
            if (manifest)
            {
                BEAST_EXPECT(
                    cache.applyManifest(
                        std::move(*manifest),
                        ManifestRateLimitCapPolicy::Capped) ==
                    ManifestDisposition::accepted);
            }
        }

        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const snapshot = unpack(overlay.getManifestsMessage());
        BEAST_EXPECT(snapshot.has_value());
        if (!snapshot)
            return;

        BEAST_EXPECT(snapshot->list_size() > 0);
        BEAST_EXPECT(
            static_cast<std::size_t>(snapshot->list_size()) < trustedCount);
        BEAST_EXPECT(
            Message::messageSize(*snapshot) <= maximumManifestsMessageSize);
        for (auto const& entry : snapshot->list())
        {
            auto manifest = deserializeManifest(entry.stobject());
            BEAST_EXPECT(manifest.has_value());
            if (manifest)
                BEAST_EXPECT(trustedMasters.contains(manifest->masterKey));
        }
    }

public:
    void
    run() override
    {
        testBoundedReceive();
        testRelaySkipIntersection();
        testSnapshotByteBudget();
    }
};

BEAST_DEFINE_TESTSUITE(manifest_relay, overlay, ripple);

}  // namespace test
}  // namespace ripple
