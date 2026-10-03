//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 Ripple Labs Inc.

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
#include <xrpld/core/ConfigSections.h>
#include <xrpld/overlay/Compression.h>
#include <xrpld/overlay/Message.h>
#include <xrpld/overlay/detail/OverlayImpl.h>
#include <xrpld/overlay/detail/PeerImp.h>
#include <xrpld/shamap/SHAMapInnerNode.h>
#include <xrpld/shamap/SHAMapNodeID.h>
#include <xrpl/basics/make_SSLContext.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/messages.h>

#include <optional>

namespace ripple {
namespace test {

class GetObjectByHash_test : public beast::unit_test::suite
{
    using socket_type = boost::asio::ip::tcp::socket;
    using middle_type = boost::beast::tcp_stream;
    using stream_type = boost::beast::ssl_stream<middle_type>;

    class PeerTest : public PeerImp
    {
    public:
        std::vector<std::shared_ptr<Message>> sent;

        PeerTest(
            Application& app,
            std::shared_ptr<PeerFinder::Slot> const& slot,
            http_request_type&& request,
            PublicKey const& publicKey,
            ProtocolVersion protocol,
            Resource::Consumer consumer,
            std::unique_ptr<GetObjectByHash_test::stream_type>&& stream_ptr,
            OverlayImpl& overlay)
            : PeerImp(
                  app,
                  1,
                  slot,
                  std::move(request),
                  publicKey,
                  protocol,
                  consumer,
                  std::move(stream_ptr),
                  overlay)
        {
        }

        void
        run() override
        {
        }

        void
        send(std::shared_ptr<Message> const& m) override
        {
            sent.push_back(m);
        }
    };

    static auto
    nullBackend(std::unique_ptr<Config> cfg)
    {
        cfg->LEDGER_HISTORY = 8;
        auto& section = cfg->section(ConfigSection::nodeDatabase());
        section.set("type", "rwdb");
        section.set("path", "main");
        return cfg;
    }

    std::shared_ptr<PeerTest>
    makePeer(jtx::Env& env)
    {
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        http_request_type request;
        auto stream_ptr = std::make_unique<stream_type>(
            socket_type(env.app().getIOContext()), *context_);
        beast::IP::Endpoint const local(
            boost::asio::ip::make_address("172.16.0.1"));
        beast::IP::Endpoint const remote(
            boost::asio::ip::make_address("172.16.0.2"));
        PublicKey const key(std::get<0>(randomKeyPair(KeyType::ed25519)));
        auto consumer = overlay.resourceManager().newInboundEndpoint(remote);
        auto slot = overlay.peerFinder().new_inbound_slot(local, remote);
        auto peer = std::make_shared<PeerTest>(
            env.app(),
            slot,
            std::move(request),
            key,
            ProtocolVersion{1, 7},
            consumer,
            std::move(stream_ptr),
            overlay);
        overlay.add_active(peer);
        return peer;
    }

    std::optional<protocol::TMGetObjectByHash>
    parseReply(std::shared_ptr<Message> const& message)
    {
        if (!message)
            return {};
        auto const& buf = message->getBuffer(compression::Compressed::Off);
        if (buf.size() <= compression::headerBytes)
            return {};
        protocol::TMGetObjectByHash reply;
        if (!reply.ParseFromArray(
                buf.data() + compression::headerBytes,
                static_cast<int>(buf.size() - compression::headerBytes)))
            return {};
        return reply;
    }

    std::shared_ptr<boost::asio::ssl::context> context_ = make_SSLContext("");

public:
    void
    testServesLinkedAndCachedNodes()
    {
        testcase(
            "GetObjectByHash serves cache, linked tree, and ledger header");

        using namespace jtx;
        Env env(*this, envconfig(nullBackend));
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env(pay(alice, bob, XRP(1)));
        env.close();

        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        if (!BEAST_EXPECT(ledger))
            return;

        auto const stateHash = ledger->stateMap().getHash().as_uint256();
        auto const txHash = ledger->txMap().getHash().as_uint256();
        auto const headerHash = ledger->info().hash;
        auto const seq = ledger->info().seq;
        auto const rootId = SHAMapNodeID{}.getRawString();

        auto peer = makePeer(env);
        auto sendQuery = [&](auto&& fill) {
            peer->sent.clear();
            auto packet = std::make_shared<protocol::TMGetObjectByHash>();
            packet->set_query(true);
            fill(*packet);
            // Production dispatch resets fee_ in onMessageBegin before
            // onMessage. Calling onMessage alone leaves a moderate burden
            // charge in place and Debug XRPL_ASSERT fires on a later
            // malformed request with a smaller fee.
            auto const size = packet->ByteSizeLong();
            peer->onMessageBegin(
                protocol::mtGET_OBJECTS, packet, size, size, false);
            peer->onMessage(packet);
            peer->onMessageEnd(protocol::mtGET_OBJECTS, packet);
        };

        // TreeNodeCache hit: hash only, no nodeid.
        sendQuery([&](protocol::TMGetObjectByHash& packet) {
            packet.set_type(protocol::TMGetObjectByHash::otSTATE_NODE);
            auto& obj = *packet.add_objects();
            obj.set_hash(stateHash.begin(), stateHash.size());
            obj.set_ledgerseq(seq);
        });
        if (auto const cached =
                env.app().getNodeFamily().getTreeNodeCache()->fetch(stateHash))
        {
            if (!BEAST_EXPECT(peer->sent.size() == 1))
                return;
            auto const reply = parseReply(peer->sent.front());
            if (!BEAST_EXPECT(reply && reply->objects_size() == 1))
                return;
            BEAST_EXPECT(uint256{reply->objects(0).hash()} == stateHash);
        }

        // After dropping the interned cache, the same request needs a nodeid
        // so fetchLinkedTreeNode can walk the retained SHAMap.
        env.app().getNodeFamily().getTreeNodeCache()->reset();
        sendQuery([&](protocol::TMGetObjectByHash& packet) {
            packet.set_type(protocol::TMGetObjectByHash::otSTATE_NODE);
            auto& obj = *packet.add_objects();
            obj.set_hash(stateHash.begin(), stateHash.size());
            obj.set_ledgerseq(seq);
        });
        {
            if (!BEAST_EXPECT(peer->sent.size() == 1))
                return;
            auto const reply = parseReply(peer->sent.front());
            if (!BEAST_EXPECT(reply))
                return;
            BEAST_EXPECT(reply->objects_size() == 0);
        }

        sendQuery([&](protocol::TMGetObjectByHash& packet) {
            packet.set_type(protocol::TMGetObjectByHash::otSTATE_NODE);
            auto& obj = *packet.add_objects();
            obj.set_hash(stateHash.begin(), stateHash.size());
            obj.set_nodeid(rootId);
            obj.set_ledgerseq(seq);
        });
        {
            if (!BEAST_EXPECT(peer->sent.size() == 1))
                return;
            auto const reply = parseReply(peer->sent.front());
            if (!BEAST_EXPECT(reply && reply->objects_size() == 1))
                return;
            BEAST_EXPECT(uint256{reply->objects(0).hash()} == stateHash);
            BEAST_EXPECT(reply->objects(0).index() == rootId);
            BEAST_EXPECT(reply->objects(0).ledgerseq() == seq);
        }

        // Transaction map root, found after the state-map miss.
        sendQuery([&](protocol::TMGetObjectByHash& packet) {
            packet.set_type(protocol::TMGetObjectByHash::otTRANSACTION_NODE);
            auto& obj = *packet.add_objects();
            obj.set_hash(txHash.begin(), txHash.size());
            obj.set_nodeid(rootId);
            obj.set_ledgerseq(seq);
        });
        {
            if (!BEAST_EXPECT(peer->sent.size() == 1))
                return;
            auto const reply = parseReply(peer->sent.front());
            if (!BEAST_EXPECT(reply && reply->objects_size() == 1))
                return;
            BEAST_EXPECT(uint256{reply->objects(0).hash()} == txHash);
        }

        // Linked walk finds the node, hash does not match, object omitted.
        uint256 const wrong{2};
        sendQuery([&](protocol::TMGetObjectByHash& packet) {
            packet.set_type(protocol::TMGetObjectByHash::otSTATE_NODE);
            auto& obj = *packet.add_objects();
            obj.set_hash(wrong.begin(), wrong.size());
            obj.set_nodeid(rootId);
            obj.set_ledgerseq(seq);
        });
        {
            if (!BEAST_EXPECT(peer->sent.size() == 1))
                return;
            auto const reply = parseReply(peer->sent.front());
            if (!BEAST_EXPECT(reply))
                return;
            BEAST_EXPECT(reply->objects_size() == 0);
        }

        // Ledger header fallback when the node store and tree caches miss.
        sendQuery([&](protocol::TMGetObjectByHash& packet) {
            packet.set_type(protocol::TMGetObjectByHash::otLEDGER);
            auto& obj = *packet.add_objects();
            obj.set_hash(headerHash.begin(), headerHash.size());
            obj.set_ledgerseq(seq);
        });
        {
            if (!BEAST_EXPECT(peer->sent.size() == 1))
                return;
            auto const reply = parseReply(peer->sent.front());
            if (!BEAST_EXPECT(reply && reply->objects_size() == 1))
                return;
            BEAST_EXPECT(uint256{reply->objects(0).hash()} == headerHash);
            BEAST_EXPECT(!reply->objects(0).data().empty());
        }

        // Malformed ledger hash is rejected before a reply is sent.
        sendQuery([&](protocol::TMGetObjectByHash& packet) {
            packet.set_type(protocol::TMGetObjectByHash::otLEDGER);
            packet.set_ledgerhash("short");
            auto& obj = *packet.add_objects();
            obj.set_hash(headerHash.begin(), headerHash.size());
        });
        BEAST_EXPECT(peer->sent.empty());
    }

    void
    run() override
    {
        testServesLinkedAndCachedNodes();
    }
};

BEAST_DEFINE_TESTSUITE(GetObjectByHash, overlay, ripple);

}  // namespace test
}  // namespace ripple
