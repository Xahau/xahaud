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

    static std::string
    makeManifest()
    {
        auto const [masterPublic, masterSecret] =
            randomKeyPair(KeyType::ed25519);
        auto const [signingPublic, signingSecret] =
            randomKeyPair(KeyType::secp256k1);

        STObject st{sfGeneric};
        st[sfSequence] = 0;
        st[sfPublicKey] = masterPublic;
        st[sfSigningPubKey] = signingPublic;
        sign(st, HashPrefix::manifest, KeyType::secp256k1, signingSecret);
        sign(
            st,
            HashPrefix::manifest,
            KeyType::ed25519,
            masterSecret,
            sfMasterSignature);

        Serializer serialized;
        st.add(serialized);
        return {static_cast<char const*>(serialized.data()), serialized.size()};
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

public:
    void
    run() override
    {
        testcase("bounded receive, source suppression, and peer snapshot");

        jtx::Env env{*this};
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const source = addPeer(env);
        auto const destination = addPeer(env);

        auto incoming = std::make_shared<protocol::TMManifests>();
        for (std::size_t i = 0; i < kMaxManifestsPerMessage + 1; ++i)
            incoming->add_list()->set_stobject(makeManifest());

        auto const before = env.app().validatorManifests().sequence();
        overlay.onManifests(incoming, source);

        BEAST_EXPECT(
            env.app().validatorManifests().sequence() ==
            before + kMaxManifestsPerMessage);
        BEAST_EXPECT(source->sent_.empty());
        BEAST_EXPECT(destination->sent_.size() == 1);

        if (destination->sent_.size() == 1)
        {
            auto const relayed = unpack(destination->sent_.front());
            BEAST_EXPECT(relayed.has_value());
            if (relayed)
                BEAST_EXPECT(relayed->list_size() == kMaxManifestsPerMessage);
        }

        auto const snapshot = unpack(overlay.getManifestsMessage());
        BEAST_EXPECT(snapshot.has_value());
        if (snapshot)
            BEAST_EXPECT(snapshot->list_size() == kMaxManifestsPerMessage);
    }
};

BEAST_DEFINE_TESTSUITE(manifest_relay, overlay, ripple);

}  // namespace test
}  // namespace ripple
