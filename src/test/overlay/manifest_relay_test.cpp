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
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/overlay/detail/OverlayImpl.h>
#include <xrpld/overlay/detail/PeerImp.h>
#include <xrpld/peerfinder/detail/SlotImp.h>
#include <xrpl/basics/make_SSLContext.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/STExchange.h>
#include <xrpl/protocol/Sign.h>

#include <limits>
#include <optional>

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
        std::optional<PublicKey> signingKey;
    };

    ManifestData
    makeManifest(
        SecretKey const& masterSecret,
        std::pair<PublicKey, SecretKey> const& signing,
        std::uint32_t const sequence,
        bool const large = false)
    {
        auto const masterPublic =
            derivePublicKey(KeyType::ed25519, masterSecret);

        STObject st{sfGeneric};
        st[sfSequence] = sequence;
        st[sfPublicKey] = masterPublic;
        st[sfSigningPubKey] = signing.first;
        if (large)
        {
            st[sfDomain] =
                makeSlice(std::string(63, 'a') + "." + std::string(63, 'b'));
        }
        sign(st, HashPrefix::manifest, KeyType::secp256k1, signing.second);
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
        return {bytes, masterPublic, signing.first};
    }

    ManifestData
    makeManifest(
        SecretKey const& masterSecret,
        std::uint32_t const sequence,
        bool const large = false)
    {
        return makeManifest(
            masterSecret, randomKeyPair(KeyType::secp256k1), sequence, large);
    }

    ManifestData
    makeRevocation(SecretKey const& masterSecret)
    {
        auto const masterPublic =
            derivePublicKey(KeyType::ed25519, masterSecret);

        STObject st{sfGeneric};
        st[sfSequence] = std::numeric_limits<std::uint32_t>::max();
        st[sfPublicKey] = masterPublic;
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
        BEAST_EXPECT(manifest && manifest->revoked());
        return {bytes, masterPublic, std::nullopt};
    }

    ManifestData
    makeManifest(bool const large = false)
    {
        return makeManifest(randomSecretKey(), 0, large);
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

    static std::optional<protocol::TMManifests>
    unpackFirst(std::vector<std::shared_ptr<Message>> const& messages)
    {
        if (messages.empty())
            return std::nullopt;
        return unpack(messages.front());
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
    installCandidate(jtx::Env& env, ManifestData const& candidate)
    {
        auto const publisherMasterSecret = randomSecretKey();
        auto const publisherMaster =
            derivePublicKey(KeyType::ed25519, publisherMasterSecret);
        auto const publisherSigning = randomKeyPair(KeyType::secp256k1);
        auto const publisherManifest =
            makeManifest(publisherMasterSecret, publisherSigning, 1);
        auto const listed = makeManifest();

        BEAST_EXPECT(env.app().validators().load(
            {}, {}, std::vector<std::string>{strHex(publisherMaster)}));

        auto const expiration = (env.timeKeeper().now() + std::chrono::hours{1})
                                    .time_since_epoch()
                                    .count();
        std::string const payload =
            "{\"sequence\":1,\"expiration\":" + std::to_string(expiration) +
            ",\"validators\":[{\"validation_public_key\":\"" +
            strHex(listed.masterKey) +
            "\"}],\"candidates\":[{\"validation_public_key\":\"" +
            strHex(candidate.masterKey) + "\",\"manifest\":\"" +
            base64_encode(candidate.serialized) + "\"}]}";
        auto const blob = base64_encode(payload);
        auto const signature = strHex(sign(
            publisherSigning.first,
            publisherSigning.second,
            makeSlice(payload)));

        BEAST_EXPECT(
            env.app()
                .validators()
                .applyLists(
                    base64_encode(publisherManifest.serialized),
                    1,
                    {{blob, signature, {}}},
                    "manifest_relay_test")
                .bestDisposition() == ListDisposition::accepted);
        auto const policy =
            env.app().validators().manifestPolicy(candidate.masterKey);
        BEAST_EXPECT(!policy.consensusListed);
        BEAST_EXPECT(policy.publisherCandidate);
        BEAST_EXPECT(policy.relayEligible());
    }

    void
    testCandidateUpdateRelays()
    {
        testcase("publisher candidate rotations and revocations relay");

        jtx::Env env{*this};
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const source = addPeer(env);
        auto const destination = addPeer(env);
        auto const masterSecret = randomSecretKey();
        auto const published = makeManifest(masterSecret, 1);
        auto const rotation = makeManifest(masterSecret, 2);
        auto const revocation = makeRevocation(masterSecret);

        installCandidate(env, published);
        BEAST_EXPECT(!env.app().validators().listed(published.masterKey));
        BEAST_EXPECT(!env.app().validators().trusted(published.masterKey));
        BEAST_EXPECT(
            env.app().validatorManifests().getSequence(published.masterKey) ==
            1);

        auto send = [&](ManifestData const& data) {
            auto incoming = std::make_shared<protocol::TMManifests>();
            incoming->add_list()->set_stobject(data.serialized);
            overlay.onManifests(incoming, source);
        };

        send(rotation);
        expectBundle(destination, 1);
        BEAST_EXPECT(
            env.app().validatorManifests().getSequence(published.masterKey) ==
            2);
        BEAST_EXPECT(
            env.app().validatorManifests().getSigningKey(published.masterKey) ==
            rotation.signingKey);
        BEAST_EXPECT(!env.app().validators().listed(published.masterKey));
        BEAST_EXPECT(!env.app().validators().trusted(published.masterKey));

        auto snapshot = unpackFirst(overlay.getManifestsMessages());
        BEAST_EXPECT(snapshot.has_value());
        if (snapshot)
        {
            BEAST_EXPECT(std::any_of(
                snapshot->list().begin(),
                snapshot->list().end(),
                [&](auto const& entry) {
                    return entry.stobject() == rotation.serialized;
                }));
        }

        destination->sent_.clear();
        send(revocation);
        expectBundle(destination, 1);
        BEAST_EXPECT(
            !env.app().validatorManifests().getSequence(published.masterKey));
        BEAST_EXPECT(
            env.app().validatorManifests().revoked(published.masterKey));
        BEAST_EXPECT(!env.app().validators().listed(published.masterKey));
        BEAST_EXPECT(!env.app().validators().trusted(published.masterKey));
    }

    void
    testCandidateCannotRollbackOrdinaryHighWater()
    {
        testcase("candidate cannot roll back ordinary manifest high-water");

        jtx::Env env{*this};
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const source = addPeer(env);
        auto const destination = addPeer(env);
        auto const masterSecret = randomSecretKey();
        auto const published = makeManifest(masterSecret, 1);
        auto const staleRotation = makeManifest(masterSecret, 2);
        auto const revocation = makeRevocation(masterSecret);

        auto ordinary = deserializeManifest(revocation.serialized);
        BEAST_EXPECT(ordinary.has_value());
        if (ordinary)
        {
            BEAST_EXPECT(
                env.app().validatorManifests().applyManifest(
                    std::move(*ordinary), ManifestRetention::evictable) ==
                ManifestDisposition::accepted);
        }
        installCandidate(env, published);

        auto incoming = std::make_shared<protocol::TMManifests>();
        incoming->add_list()->set_stobject(staleRotation.serialized);
        overlay.onManifests(incoming, source);

        BEAST_EXPECT(destination->sent_.empty());
        BEAST_EXPECT(
            env.app().validatorManifests().revoked(published.masterKey));

        auto const snapshot = unpackFirst(overlay.getManifestsMessages());
        BEAST_EXPECT(snapshot.has_value());
        if (snapshot)
        {
            BEAST_EXPECT(std::any_of(
                snapshot->list().begin(),
                snapshot->list().end(),
                [&](auto const& entry) {
                    return entry.stobject() == revocation.serialized;
                }));
            BEAST_EXPECT(std::none_of(
                snapshot->list().begin(),
                snapshot->list().end(),
                [&](auto const& entry) {
                    return entry.stobject() == staleRotation.serialized;
                }));
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
        expectBundle(destination, 1);

        auto const snapshot = unpackFirst(overlay.getManifestsMessages());
        BEAST_EXPECT(snapshot.has_value());
        if (snapshot)
        {
            BEAST_EXPECT(snapshot->list_size() == kMaxManifestsPerMessage + 1);
            BEAST_EXPECT(
                Message::messageSize(*snapshot) <= maximumManifestsMessageSize);
        }
    }

    void
    testTotalEntryLimit()
    {
        testcase("total entry limit bounds malformed work");

        jtx::Env env{*this};
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const source = addPeer(env);
        auto const destination = addPeer(env);

        auto incoming = std::make_shared<protocol::TMManifests>();
        for (std::size_t i = 0; i < kMaxManifestEntriesPerMessage + 1; ++i)
            incoming->add_list()->set_stobject("");

        BEAST_EXPECT(incoming->IsInitialized());
        auto const before = env.app().validatorManifests().sequence();
        overlay.onManifests(incoming, source);

        BEAST_EXPECT(env.app().validatorManifests().sequence() == before);
        BEAST_EXPECT(source->sent_.empty());
        BEAST_EXPECT(destination->sent_.empty());
    }

    void
    testKnownUpdateRelays()
    {
        testcase("known unlisted rotations relay");

        jtx::Env env{*this};
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const source = addPeer(env);
        auto const destination = addPeer(env);
        auto const masterSecret = randomSecretKey();
        auto const first = makeManifest(masterSecret, 0);
        auto const second = makeManifest(masterSecret, 1);

        auto incoming = std::make_shared<protocol::TMManifests>();
        incoming->add_list()->set_stobject(first.serialized);
        overlay.onManifests(incoming, source);

        BEAST_EXPECT(source->sent_.empty());
        BEAST_EXPECT(destination->sent_.empty());

        incoming = std::make_shared<protocol::TMManifests>();
        incoming->add_list()->set_stobject(second.serialized);
        overlay.onManifests(incoming, source);

        BEAST_EXPECT(source->sent_.empty());
        expectBundle(destination, 1);
    }

    void
    testRevocationRelay()
    {
        testcase("revocation relay follows retained and listed policy");

        auto const retainedSecret = randomSecretKey();
        auto const retained = makeManifest(retainedSecret, 0);
        auto const retainedRevocation = makeRevocation(retainedSecret);
        auto const firstSeenRevocation = makeRevocation(randomSecretKey());
        auto const listedRevocation = makeRevocation(randomSecretKey());

        jtx::Env env{
            *this,
            jtx::envconfig([&listedRevocation](std::unique_ptr<Config> config) {
                config->section("validators")
                    .append(toBase58(
                        TokenType::NodePublic, listedRevocation.masterKey));
                return config;
            })};
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const source = addPeer(env);
        auto const destination = addPeer(env);

        auto send = [&](ManifestData const& data) {
            auto incoming = std::make_shared<protocol::TMManifests>();
            incoming->add_list()->set_stobject(data.serialized);
            overlay.onManifests(incoming, source);
        };

        send(retained);
        BEAST_EXPECT(destination->sent_.empty());

        send(retainedRevocation);
        expectBundle(destination, 1);
        BEAST_EXPECT(
            env.app().validatorManifests().revoked(retained.masterKey));
        destination->sent_.clear();

        send(firstSeenRevocation);
        BEAST_EXPECT(destination->sent_.empty());
        BEAST_EXPECT(env.app().validatorManifests().revoked(
            firstSeenRevocation.masterKey));

        send(listedRevocation);
        expectBundle(destination, 1);
        BEAST_EXPECT(
            env.app().validatorManifests().revoked(listedRevocation.masterKey));
        BEAST_EXPECT(source->sent_.empty());
    }

    void
    testEvictedManifestStopsLocally()
    {
        testcase("reaccepted evicted manifest does not live relay");

        jtx::Env env{*this};
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const source = addPeer(env);
        auto const destination = addPeer(env);
        auto const target = makeManifest();

        auto incoming = std::make_shared<protocol::TMManifests>();
        incoming->add_list()->set_stobject(target.serialized);
        overlay.onManifests(incoming, source);
        BEAST_EXPECT(source->sent_.empty());
        BEAST_EXPECT(destination->sent_.empty());

        constexpr std::size_t untrustedCacheLimit = 1000;
        hash_set<PublicKey> activeSigningKeys;
        activeSigningKeys.reserve(untrustedCacheLimit - 1);
        auto& cache = env.app().validatorManifests();
        for (std::size_t i = 1; i < untrustedCacheLimit; ++i)
        {
            auto const filler = makeManifest();
            BEAST_EXPECT(filler.signingKey.has_value());
            if (filler.signingKey)
                activeSigningKeys.insert(*filler.signingKey);
            auto manifest = deserializeManifest(filler.serialized);
            BEAST_EXPECT(manifest.has_value());
            if (manifest)
            {
                BEAST_EXPECT(
                    cache.applyManifest(
                        std::move(*manifest), ManifestRetention::evictable) ==
                    ManifestDisposition::accepted);
            }
        }

        auto const replacement = makeManifest();
        auto replacementManifest = deserializeManifest(replacement.serialized);
        BEAST_EXPECT(replacementManifest.has_value());
        if (replacementManifest)
        {
            BEAST_EXPECT(
                cache
                    .applyManifestWithEviction(
                        std::move(*replacementManifest), activeSigningKeys)
                    .disposition == ManifestDisposition::accepted);
        }
        BEAST_EXPECT(!cache.getSequence(target.masterKey));

        incoming = std::make_shared<protocol::TMManifests>();
        incoming->add_list()->set_stobject(target.serialized);
        overlay.onManifests(incoming, source);

        BEAST_EXPECT(cache.getSequence(target.masterKey) == 0);
        BEAST_EXPECT(source->sent_.empty());
        BEAST_EXPECT(destination->sent_.empty());
    }

    void
    testSnapshotByteBudget()
    {
        testcase("snapshot chunks every protected entry within byte budget");

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
                        std::move(*manifest), ManifestRetention::protected_) ==
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
                        std::move(*manifest), ManifestRetention::evictable) ==
                    ManifestDisposition::accepted);
            }
        }

        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const messages = overlay.getManifestsMessages();
        BEAST_EXPECT(messages.size() > 1);

        hash_set<PublicKey> seenTrusted;
        std::size_t seenUntrusted = 0;
        for (auto const& message : messages)
        {
            auto const snapshot = unpack(message);
            BEAST_EXPECT(snapshot.has_value());
            if (!snapshot)
                continue;
            BEAST_EXPECT(
                Message::messageSize(*snapshot) <= maximumManifestsMessageSize);
            BEAST_EXPECT(
                static_cast<std::size_t>(snapshot->list_size()) <=
                kMaxManifestEntriesPerMessage);
            for (auto const& entry : snapshot->list())
            {
                auto manifest = deserializeManifest(entry.stobject());
                BEAST_EXPECT(manifest.has_value());
                if (!manifest)
                    continue;
                if (trustedMasters.contains(manifest->masterKey))
                    seenTrusted.insert(manifest->masterKey);
                else
                    ++seenUntrusted;
            }
        }
        BEAST_EXPECT(seenTrusted.size() == trustedCount);
        BEAST_EXPECT(seenUntrusted <= untrustedCount);
    }

    void
    testListingChangeInvalidatesSnapshot()
    {
        testcase("retention change invalidates cached snapshot");

        jtx::Env env{*this};
        auto const target = makeManifest();
        auto manifest = deserializeManifest(target.serialized);
        BEAST_EXPECT(manifest.has_value());
        if (!manifest)
            return;

        auto& cache = env.app().validatorManifests();
        BEAST_EXPECT(
            cache.applyManifest(
                std::move(*manifest), ManifestRetention::evictable) ==
            ManifestDisposition::accepted);

        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const first = overlay.getManifestsMessages();
        BEAST_EXPECT(first.size() == 1);
        auto const manifestSeq = cache.sequence();

        BEAST_EXPECT(env.app().validators().load(
            std::nullopt,
            {toBase58(TokenType::NodePublic, target.masterKey)},
            {},
            std::nullopt));
        BEAST_EXPECT(env.app().validators().listed(target.masterKey));
        BEAST_EXPECT(cache.sequence() == manifestSeq + 1);

        auto const second = overlay.getManifestsMessages();
        BEAST_EXPECT(second.size() == 1);
        if (first.size() == 1 && second.size() == 1)
            BEAST_EXPECT(second.front() != first.front());
    }

    void
    testConfigListingPromotesRetention()
    {
        testcase("config listing promotes retained manifest");

        jtx::Env env{*this};
        auto const target = makeManifest();
        auto manifest = deserializeManifest(target.serialized);
        BEAST_EXPECT(manifest.has_value());
        if (!manifest)
            return;

        auto& cache = env.app().validatorManifests();
        BEAST_EXPECT(
            cache.applyManifest(
                std::move(*manifest), ManifestRetention::evictable) ==
            ManifestDisposition::accepted);
        auto const before = cache.sequence();

        BEAST_EXPECT(env.app().validators().load(
            std::nullopt,
            {toBase58(TokenType::NodePublic, target.masterKey)},
            {},
            std::nullopt));
        BEAST_EXPECT(env.app().validators().listed(target.masterKey));
        BEAST_EXPECT(cache.sequence() == before + 1);

        cache.setRetention(target.masterKey, ManifestRetention::protected_);
        BEAST_EXPECT(cache.sequence() == before + 1);
    }

public:
    void
    run() override
    {
        testBoundedReceive();
        testTotalEntryLimit();
        testKnownUpdateRelays();
        testCandidateUpdateRelays();
        testCandidateCannotRollbackOrdinaryHighWater();
        testRevocationRelay();
        testEvictedManifestStopsLocally();
        testSnapshotByteBudget();
        testListingChangeInvalidatesSnapshot();
        testConfigListingPromotesRetention();
    }
};

BEAST_DEFINE_TESTSUITE(manifest_relay, overlay, ripple);

}  // namespace test
}  // namespace ripple
