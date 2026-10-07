//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/core/JobQueue.h>
#include <xrpld/overlay/detail/OverlayImpl.h>
#include <xrpld/overlay/detail/XUSH.h>
#include <xrpl/beast/net/IPAddressConversion.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/resource/ResourceManager.h>
#include <xrpl/resource/detail/Tuning.h>

#include <boost/asio/ip/udp.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ripple {
namespace test {

// Tests the UDP Superhighway (XUSH) handling in OverlayImpl: inbound datagram
// processing, outbound publishing and the XUSHPEER advertisement timer.
class OverlayXUSH_test : public beast::unit_test::suite
{
    using udp = boost::asio::ip::udp;

    static std::unique_ptr<Config>
    highway(std::unique_ptr<Config> cfg)
    {
        cfg->UDP_HIGHWAY = true;
        return cfg;
    }

    static beast::IP::Endpoint
    ep(std::string const& s)
    {
        return beast::IP::Endpoint::from_string(s);
    }

    static std::set<beast::IP::Endpoint>
    asSet(std::vector<beast::IP::Endpoint> const& v)
    {
        return {v.begin(), v.end()};
    }

    static Slice
    slice(Buffer const& b)
    {
        return Slice(b.data(), b.size());
    }

    static std::string
    str(Buffer const& b)
    {
        return std::string(reinterpret_cast<char const*>(b.data()), b.size());
    }

    static Buffer
    serialize(STTx const& stx)
    {
        Serializer s;
        stx.add(s);
        return Buffer(s.data(), s.size());
    }

    // A pseudo-transaction, which servers never submit
    static STTx
    pseudoTx()
    {
        return STTx(ttAMENDMENT, [](auto& obj) {
            obj.setAccountID(sfAccount, AccountID());
            obj.setFieldH256(sfAmendment, uint256(2));
            obj.setFieldU32(sfLedgerSequence, 5);
        });
    }

    // A local UDP socket that highway datagrams can be sent to
    struct Sink
    {
        boost::asio::io_context io;
        udp::socket socket;

        explicit Sink(boost::asio::ip::address const& addr)
            : socket(io, udp::endpoint(addr, 0))
        {
            socket.non_blocking(true);
        }

        beast::IP::Endpoint
        endpoint() const
        {
            auto const local = socket.local_endpoint();
            return beast::IP::Endpoint(local.address(), local.port());
        }

        void
        sendTo(Buffer const& datagram, std::uint16_t port)
        {
            socket.send_to(
                boost::asio::buffer(datagram.data(), datagram.size()),
                udp::endpoint(socket.local_endpoint().address(), port));
        }

        std::optional<std::string>
        receive(std::chrono::milliseconds timeout = std::chrono::seconds(5))
        {
            auto const deadline = std::chrono::steady_clock::now() + timeout;
            std::array<char, 4096> buf;
            while (std::chrono::steady_clock::now() < deadline)
            {
                udp::endpoint from;
                boost::system::error_code ec;
                auto const n =
                    socket.receive_from(boost::asio::buffer(buf), from, 0, ec);
                if (!ec)
                    return std::string(buf.data(), n);
                if (ec != boost::asio::error::would_block &&
                    ec != boost::asio::error::try_again)
                    return std::nullopt;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return std::nullopt;
        }

        // Receives datagrams until one of the given type arrives
        std::optional<std::string>
        receive(xush::MessageType type)
        {
            while (auto d = receive())
            {
                if (xush::classify(makeSlice(*d)) == type)
                    return d;
            }
            return std::nullopt;
        }
    };

    struct Fixture
    {
        jtx::Env env;
        OverlayImpl& overlay;
        PeerFinder::Manager& peerFinder;
        Resource::Manager& resources;

        Fixture(beast::unit_test::suite& suite, bool enabled)
            : env(suite, enabled ? jtx::envconfig(highway) : jtx::envconfig())
            , overlay(dynamic_cast<OverlayImpl&>(env.app().overlay()))
            , peerFinder(overlay.peerFinder())
            , resources(env.app().getResourceManager())
        {
        }

        // Delivers a datagram as if it had been received from `remote`
        void
        deliver(Buffer const& datagram, beast::IP::Endpoint const& remote)
        {
            overlay.processXUSH(
                str(datagram), beast::IP::to_asio_endpoint(remote));
        }

        void
        deliver(
            std::vector<Buffer> const& datagrams,
            beast::IP::Endpoint const& remote)
        {
            for (auto const& d : datagrams)
                deliver(d, remote);
        }

        // Tracks the resource balance charged to `remote`
        Resource::Consumer
        probe(beast::IP::Endpoint const& remote)
        {
            return resources.newInboundEndpoint(remote);
        }

        std::set<beast::IP::Endpoint>
        targets()
        {
            return asSet(peerFinder.highway_targets(1000));
        }

        // Whether `probe` was charged at least `fee` since `before`. Balances
        // are normalized by the decay window, so a charge of `fee` raises the
        // balance by at least fee / decayWindowSeconds.
        static bool
        charged(Resource::Consumer& probe, int before, int fee)
        {
            return probe.balance() - before >=
                fee / Resource::decayWindowSeconds;
        }

        bool
        isBad(uint256 const& txid)
        {
            env.app().getJobQueue().rendezvous();
            return env.app().getHashRouter().getFlags(txid) & SF_BAD;
        }
    };

    void
    testDisabled()
    {
        testcase("disabled");
        using namespace jtx;
        Fixture f(*this, false);
        auto const remote = ep("203.0.113.1:51235");

        // Nothing is learned or sent while the highway is disabled
        f.deliver(xush::encodePeers({ep("8.8.8.8:51235")}), remote);
        BEAST_EXPECT(f.targets().empty());

        Sink sink(boost::asio::ip::make_address("127.0.0.1"));
        f.peerFinder.addFixedPeer("sink", {sink.endpoint()});
        BEAST_EXPECT(f.targets().empty());

        Account const alice("alice");
        f.env.fund(XRP(10000), alice);
        f.env.close();

        Buffer const blob(64);
        f.overlay.publishTxXUSH(slice(blob), uint256(1));
        BEAST_EXPECT(!sink.receive(std::chrono::milliseconds(200)));
    }

    void
    testPeers()
    {
        testcase("XUSHPEER");
        Fixture f(*this, true);
        auto const remote = ep("203.0.113.2:51235");
        auto probe = f.probe(remote);

        BEAST_EXPECT(f.targets().empty());

        f.deliver(
            xush::encodePeers({ep("8.8.8.8:51235"), ep("1.1.1.1:51235")}),
            remote);
        BEAST_EXPECT(
            f.targets() == asSet({ep("8.8.8.8:51235"), ep("1.1.1.1:51235")}));
        auto const balance = probe.balance();

        // Unknown datagram types are ignored and cost almost nothing
        f.deliver(Buffer("XUSHPING", 8), remote);
        BEAST_EXPECT(!Fixture::charged(probe, balance, 100));

        // A malformed advertisement is charged for
        Buffer malformed(xush::tagSize + 2);
        std::memcpy(malformed.data(), "XUSHPEER\x05\x00", malformed.size());
        f.deliver(malformed, remote);
        BEAST_EXPECT(Fixture::charged(probe, balance, 200));
        BEAST_EXPECT(f.targets().size() == 2);
    }

    void
    testTransaction()
    {
        testcase("XUSHTXNF transaction");
        using namespace jtx;
        Fixture f(*this, true);
        auto& env = f.env;
        auto const remote = ep("203.0.113.3:51235");

        Account const alice("alice");
        Account const bob("bob");
        env.fund(XRP(10000), alice, bob);
        env.close();

        auto const jt = env.jt(pay(alice, bob, XRP(100)));
        auto const txid = jt.stx->getTransactionID();

        // Fragments arrive out of order and are reassembled
        auto datagrams = xush::encodeTxn(slice(serialize(*jt.stx)), txid, 40);
        BEAST_EXPECT(datagrams.size() > 2);
        std::reverse(datagrams.begin(), datagrams.end());
        f.deliver(datagrams, remote);
        env.app().getJobQueue().rendezvous();

        BEAST_EXPECT(!f.isBad(txid));
        env.close();
        BEAST_EXPECT(env.balance(bob) == XRP(10100));

        // Delivering it again is harmless
        f.deliver(datagrams, remote);
        env.app().getJobQueue().rendezvous();
        env.close();
        BEAST_EXPECT(env.balance(bob) == XRP(10100));
    }

    void
    testBadTransactions()
    {
        testcase("XUSHTXNF bad transactions");
        using namespace jtx;
        Fixture f(*this, true);
        auto& env = f.env;
        auto const remote = ep("203.0.113.4:51235");
        auto probe = f.probe(remote);

        Account const alice("alice");
        Account const bob("bob");
        env.fund(XRP(10000), alice, bob);
        env.close();

        auto const good = env.jt(pay(alice, bob, XRP(100)));
        auto const blob = serialize(*good.stx);
        auto const txid = good.stx->getTransactionID();

        // The ID in the header must match the transaction
        auto balance = probe.balance();
        f.deliver(xush::encodeTxn(slice(blob), uint256(1)), remote);
        BEAST_EXPECT(Fixture::charged(probe, balance, 400));
        BEAST_EXPECT(!f.isBad(txid));

        // Garbage that doesn't parse as a transaction
        balance = probe.balance();
        f.deliver(
            xush::encodeTxn(makeSlice(std::string("garbage")), uint256(2)),
            remote);
        BEAST_EXPECT(Fixture::charged(probe, balance, 400));

        // Fragments of one transaction that disagree with each other
        balance = probe.balance();
        f.deliver(xush::encodeTxn(slice(blob), uint256(3), 40).front(), remote);
        f.deliver(xush::encodeTxn(slice(blob), uint256(3), 60).front(), remote);
        BEAST_EXPECT(Fixture::charged(probe, balance, 400));

        // A malformed fragment
        balance = probe.balance();
        f.deliver(Buffer("XUSHTXNF", 8), remote);
        BEAST_EXPECT(Fixture::charged(probe, balance, 200));

        // Pseudo-transactions are never accepted
        balance = probe.balance();
        auto const pseudo = pseudoTx();
        f.deliver(
            xush::encodeTxn(
                slice(serialize(pseudo)), pseudo.getTransactionID()),
            remote);
        BEAST_EXPECT(Fixture::charged(probe, balance, 2000));

        // An expired transaction is remembered as bad
        auto const expired =
            env.jt(pay(alice, bob, XRP(100)), last_ledger_seq(1));
        auto const expiredId = expired.stx->getTransactionID();
        balance = probe.balance();
        f.deliver(
            xush::encodeTxn(slice(serialize(*expired.stx)), expiredId), remote);
        BEAST_EXPECT(f.isBad(expiredId));
        BEAST_EXPECT(Fixture::charged(probe, balance, 150));

        // A badly signed transaction is remembered as bad
        auto corrupted = good.stx->getJson(JsonOptions::none);
        corrupted[jss::TxnSignature] = std::string(128, '0');
        STTx const badSig(parse(corrupted));
        auto const badSigId = badSig.getTransactionID();
        balance = probe.balance();
        f.deliver(xush::encodeTxn(slice(serialize(badSig)), badSigId), remote);
        BEAST_EXPECT(f.isBad(badSigId));
        BEAST_EXPECT(Fixture::charged(probe, balance, 2000));

        // A known bad transaction is charged for again
        balance = probe.balance();
        f.deliver(xush::encodeTxn(slice(serialize(badSig)), badSigId), remote);
        BEAST_EXPECT(Fixture::charged(probe, balance, 150));

        env.close();
        BEAST_EXPECT(env.balance(bob) == XRP(10000));
    }

    void
    testDisconnect()
    {
        testcase("resource limit");
        Fixture f(*this, true);
        auto const remote = ep("203.0.113.5:51235");
        auto probe = f.probe(remote);

        // A sender that keeps misbehaving is ignored
        Buffer const blob(64);
        for (int i = 0; i < 10000 && probe.balance() < Resource::dropThreshold;
             ++i)
            f.deliver(xush::encodeTxn(slice(blob), uint256(1)), remote);
        BEAST_EXPECT(probe.balance() >= Resource::dropThreshold);

        f.deliver(xush::encodePeers({ep("8.8.8.8:51235")}), remote);
        BEAST_EXPECT(f.targets().empty());

        // Other senders are not affected
        f.deliver(
            xush::encodePeers({ep("8.8.8.8:51235")}), ep("203.0.113.6:51235"));
        BEAST_EXPECT(f.targets() == asSet({ep("8.8.8.8:51235")}));
    }

    void
    testPublish()
    {
        testcase("publish");
        using namespace jtx;
        Fixture f(*this, true);
        auto& env = f.env;

        Account const alice("alice");
        Account const bob("bob");
        env.fund(XRP(10000), alice, bob);
        env.close();

        Sink sink4(boost::asio::ip::make_address("127.0.0.1"));
        f.peerFinder.addFixedPeer("sink4", {sink4.endpoint()});

        std::optional<Sink> sink6;
        try
        {
            sink6.emplace(boost::asio::ip::make_address("::1"));
            f.peerFinder.addFixedPeer("sink6", {sink6->endpoint()});
        }
        catch (std::exception const&)
        {
            log << "IPv6 loopback unavailable, skipping" << std::endl;
        }

        // A transaction relayed by NetworkOPs is published on the highway
        env(pay(alice, bob, XRP(100)));
        auto const stx = env.tx();
        auto const blob = serialize(*stx);

        auto check = [&](Sink& sink) {
            auto const d = sink.receive(xush::MessageType::txn);
            if (!BEAST_EXPECT(d))
                return;
            auto const fragment = xush::decodeTxnFragment(makeSlice(*d));
            if (!BEAST_EXPECT(fragment))
                return;
            BEAST_EXPECT(fragment->txid == stx->getTransactionID());
            BEAST_EXPECT(fragment->count == 1);
            BEAST_EXPECT(fragment->payload == slice(blob));
        };
        check(sink4);
        if (sink6)
            check(*sink6);

        // Large transactions are fragmented
        Buffer big(5000);
        for (std::size_t i = 0; i < big.size(); ++i)
            big.data()[i] = static_cast<std::uint8_t>(i);
        f.overlay.publishTxXUSH(slice(big), uint256(7));

        xush::Reassembler reassembler;
        std::optional<Buffer> rebuilt;
        while (!rebuilt)
        {
            auto const d = sink4.receive(xush::MessageType::txn);
            if (!BEAST_EXPECT(d))
                break;
            auto const fragment = xush::decodeTxnFragment(makeSlice(*d));
            if (!BEAST_EXPECT(fragment && fragment->txid == uint256(7)))
                break;
            auto result = reassembler.add(
                sink4.endpoint(),
                *fragment,
                xush::Reassembler::clock_type::now());
            if (result.status == xush::Reassembler::Status::complete)
                rebuilt = std::move(result.txn);
        }
        BEAST_EXPECT(rebuilt && *rebuilt == big);

        // Transactions too large for the highway are not sent
        Buffer const huge(xush::maxTxnSize + 1);
        f.overlay.publishTxXUSH(slice(huge), uint256(8));
        f.overlay.publishTxXUSH(slice(big), uint256(9));
        auto const d = sink4.receive(xush::MessageType::txn);
        if (BEAST_EXPECT(d))
        {
            auto const fragment = xush::decodeTxnFragment(makeSlice(*d));
            BEAST_EXPECT(fragment && fragment->txid == uint256(9));
        }
    }

    void
    testSendPeers()
    {
        testcase("advertise");
        Fixture f(*this, true);

        Sink sink(boost::asio::ip::make_address("127.0.0.1"));
        f.peerFinder.addFixedPeer("sink", {sink.endpoint()});

        // Learned endpoints are advertised to targets once per second, but
        // fixed peers are never advertised.
        f.peerFinder.add_highway_peers({ep("8.8.8.8:51235")});

        auto const d = sink.receive(xush::MessageType::peers);
        if (!BEAST_EXPECT(d))
            return;
        auto const endpoints = xush::decodePeers(makeSlice(*d));
        BEAST_EXPECT(
            endpoints && asSet(*endpoints) == asSet({ep("8.8.8.8:51235")}));
    }

public:
    void
    run() override
    {
        testDisabled();
        testPeers();
        testTransaction();
        testBadTransactions();
        testDisconnect();
        testPublish();
        testSendPeers();
    }
};

BEAST_DEFINE_TESTSUITE(OverlayXUSH, overlay, ripple);

}  // namespace test
}  // namespace ripple
