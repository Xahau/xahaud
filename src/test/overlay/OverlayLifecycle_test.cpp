#include <test/jtx.h>

#include <xrpld/overlay/detail/ConnectAttempt.h>
#include <xrpld/overlay/detail/OverlayImpl.h>
#include <xrpld/overlay/detail/PeerImp.h>
#include <xrpl/basics/make_SSLContext.h>
#include <xrpl/beast/net/IPAddressConversion.h>
#include <xrpl/beast/unit_test.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <string>

namespace ripple {
namespace test {

class OverlayLifecycle_test : public beast::unit_test::suite
{
    using tcp = boost::asio::ip::tcp;
    using error_code = boost::system::error_code;
    using stream_type = boost::beast::ssl_stream<boost::beast::tcp_stream>;

    std::shared_ptr<boost::asio::ssl::context> context_ = make_SSLContext("");

    enum class Behavior {
        abortHandshake,
        stallHandshake,
        reject,
        stallResponse
    };

    // A single local endpoint on the same explicitly pumped context as the
    // real ConnectAttempt. No server thread or external network is needed.
    struct Listener
    {
        Behavior behavior;
        tcp::acceptor acceptor;
        boost::asio::ssl::stream<tcp::socket> stream;
        boost::beast::flat_buffer buffer;
        boost::beast::http::request<boost::beast::http::empty_body> request;
        boost::beast::http::response<boost::beast::http::string_body> response;
        std::array<char, 4096> drainBuffer;
        bool accepted = false;
        bool handshakeBytes = false;
        bool handshaken = false;
        bool requested = false;
        bool closedByClient = false;

        Listener(
            boost::asio::io_context& ioc,
            boost::asio::ssl::context& context,
            Behavior mode)
            : behavior(mode)
            , acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0})
            , stream(ioc, context)
        {
            acceptor.async_accept(stream.next_layer(), [this](error_code ec) {
                if (ec)
                    return;
                accepted = true;
                if (behavior == Behavior::abortHandshake)
                {
                    stream.next_layer().close(ec);
                    return;
                }
                if (behavior == Behavior::stallHandshake)
                    return drain();
                stream.async_handshake(
                    boost::asio::ssl::stream_base::server,
                    [this](error_code ec) {
                        if (ec)
                            return;
                        handshaken = true;
                        boost::beast::http::async_read(
                            stream,
                            buffer,
                            request,
                            [this](error_code ec, std::size_t) {
                                if (ec)
                                    return;
                                requested = true;
                                if (behavior == Behavior::stallResponse)
                                    return drain();
                                response.version(11);
                                response.result(boost::beast::http::status::
                                                    service_unavailable);
                                response.body() = R"({"peer-ips":[]})";
                                response.prepare_payload();
                                boost::beast::http::async_write(
                                    stream,
                                    response,
                                    [this](error_code ec, std::size_t) {
                                        if (!ec)
                                            drain();
                                    });
                            });
                    });
            });
        }

        tcp::endpoint
        endpoint() const
        {
            return acceptor.local_endpoint();
        }

        void
        drain()
        {
            // Consume transport bytes without sending any TLS or HTTP reply.
            // EOF/reset proves that the attempt itself closed the connection.
            stream.next_layer().async_read_some(
                boost::asio::buffer(drainBuffer),
                [this](error_code ec, std::size_t n) {
                    if (ec)
                    {
                        closedByClient = true;
                        return;
                    }
                    handshakeBytes = handshakeBytes || n != 0;
                    drain();
                });
        }

        void
        stop()
        {
            error_code ec;
            acceptor.close(ec);
            stream.next_layer().close(ec);
        }
    };

    template <class Predicate>
    bool
    runUntil(boost::asio::io_context& ioc, Predicate const& ready)
    {
        ioc.restart();
        auto const limit =
            std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!ready() && !ioc.stopped() &&
               std::chrono::steady_clock::now() < limit)
            ioc.run_one_until(limit);
        return ready();
    }

    std::shared_ptr<ConnectAttempt>
    makeAttempt(
        jtx::Env& env,
        boost::asio::io_context& ioc,
        tcp::endpoint endpoint)
    {
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        auto const remote = beast::IPAddressConversion::from_asio(endpoint);
        auto slot = overlay.peerFinder().new_outbound_slot(remote);
        if (!BEAST_EXPECT(slot != nullptr))
            return {};
        return std::make_shared<ConnectAttempt>(
            env.app(),
            ioc,
            endpoint,
            overlay.resourceManager().newOutboundEndpoint(remote),
            context_,
            100000,
            slot,
            env.app().journal("Peer"),
            overlay);
    }

    static void
    stopAttempt(std::weak_ptr<ConnectAttempt> const& weak)
    {
        if (auto attempt = weak.lock())
            attempt->stop();
    }

    void
    testRefused()
    {
        testcase("refused connection releases its attempt");
        jtx::Env env{*this};
        boost::asio::io_context ioc;
        // Reserve an endpoint without listening; no close/rebind race.
        tcp::acceptor reserved(ioc);
        reserved.open(tcp::v4());
        reserved.bind({boost::asio::ip::make_address("127.0.0.1"), 0});
        auto attempt = makeAttempt(env, ioc, reserved.local_endpoint());
        if (!attempt)
            return;
        std::weak_ptr<ConnectAttempt> weak = attempt;
        attempt->run();
        attempt.reset();
        ioc.run_for(std::chrono::seconds{5});
        BEAST_EXPECT(weak.expired());
        BEAST_EXPECT(ioc.stopped());
        stopAttempt(weak);
        ioc.restart();
        ioc.run_for(std::chrono::seconds{1});
    }

    void
    testRejected(Behavior behavior)
    {
        testcase(
            behavior == Behavior::reject ? "TLS peer refuses protocol upgrade"
                                         : "peer closes during TLS handshake");
        jtx::Env env{*this};
        boost::asio::io_context ioc;
        Listener server(ioc, *context_, behavior);
        auto attempt = makeAttempt(env, ioc, server.endpoint());
        if (!attempt)
            return;
        std::weak_ptr<ConnectAttempt> weak = attempt;
        attempt->run();
        attempt.reset();
        ioc.run_for(std::chrono::seconds{5});
        BEAST_EXPECT(server.accepted);
        if (behavior == Behavior::reject)
        {
            BEAST_EXPECT(server.handshaken);
            BEAST_EXPECT(server.requested);
            BEAST_EXPECT(server.request["Connect-As"] == "Peer");
            BEAST_EXPECT(server.closedByClient);
        }
        BEAST_EXPECT(weak.expired());
        BEAST_EXPECT(ioc.stopped());
        stopAttempt(weak);
        server.stop();
        ioc.restart();
        ioc.run_for(std::chrono::seconds{1});
    }

    void
    testStop()
    {
        testcase("stop cancels a pending TLS handshake and timer");
        jtx::Env env{*this};
        boost::asio::io_context ioc;
        Listener server(ioc, *context_, Behavior::stallHandshake);
        auto attempt = makeAttempt(env, ioc, server.endpoint());
        if (!attempt)
            return;
        std::weak_ptr<ConnectAttempt> weak = attempt;
        attempt->run();
        BEAST_EXPECT(runUntil(ioc, [&] { return server.handshakeBytes; }));
        // Exercise the off-strand post and repeated stop as well as cancel.
        attempt->stop();
        attempt->stop();
        attempt.reset();
        ioc.restart();
        ioc.run_for(std::chrono::seconds{5});
        BEAST_EXPECT(server.closedByClient);
        BEAST_EXPECT(weak.expired());
        BEAST_EXPECT(ioc.stopped());
        server.stop();
    }

    void
    testDeadlines()
    {
        testcase("stalled handshake and HTTP response expire");
        jtx::Env env{*this};
        boost::asio::io_context ioc;
        Listener handshake(ioc, *context_, Behavior::stallHandshake);
        Listener response(ioc, *context_, Behavior::stallResponse);
        auto first = makeAttempt(env, ioc, handshake.endpoint());
        auto second = makeAttempt(env, ioc, response.endpoint());
        if (!first || !second)
            return;
        std::weak_ptr<ConnectAttempt> firstWeak = first;
        std::weak_ptr<ConnectAttempt> secondWeak = second;
        first->run();
        second->run();
        first.reset();
        second.reset();
        // Exercise the real 15-second timers concurrently. The upper bound
        // keeps a missing wait from hanging the suite indefinitely.
        ioc.run_for(std::chrono::seconds{20});
        BEAST_EXPECT(handshake.handshakeBytes);
        BEAST_EXPECT(response.handshaken);
        BEAST_EXPECT(response.requested);
        BEAST_EXPECT(handshake.closedByClient);
        BEAST_EXPECT(response.closedByClient);
        BEAST_EXPECT(firstWeak.expired());
        BEAST_EXPECT(secondWeak.expired());
        BEAST_EXPECT(ioc.stopped());
        stopAttempt(firstWeak);
        stopAttempt(secondWeak);
        handshake.stop();
        response.stop();
        ioc.restart();
        ioc.run_for(std::chrono::seconds{1});
    }

    void
    testPeer(bool graceful)
    {
        testcase(
            graceful ? "peer handles TLS close and cancels its timer"
                     : "peer stop closes once and releases pending work");
        jtx::Env env{*this};
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        boost::asio::io_context ioc;
        tcp::acceptor acceptor(
            ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
        boost::asio::ssl::stream<tcp::socket> remote(ioc, *context_);
        auto stream = std::make_unique<stream_type>(ioc, *context_);
        stream->next_layer().socket().connect(acceptor.local_endpoint());
        acceptor.accept(remote.next_layer());
        auto const endpoint =
            beast::IPAddressConversion::from_asio(acceptor.local_endpoint());
        auto const local = beast::IPAddressConversion::from_asio(
            stream->next_layer().socket().local_endpoint());
        int handshakes = 0;
        remote.async_handshake(
            boost::asio::ssl::stream_base::server, [&](error_code ec) {
                BEAST_EXPECT(!ec);
                if (!ec)
                    ++handshakes;
            });
        stream->async_handshake(
            boost::asio::ssl::stream_base::client, [&](error_code ec) {
                BEAST_EXPECT(!ec);
                if (!ec)
                    ++handshakes;
            });
        ioc.run_for(std::chrono::seconds{5});
        if (!BEAST_EXPECT(handshakes == 2))
            return;

        auto slot = overlay.peerFinder().new_outbound_slot(endpoint);
        if (!BEAST_EXPECT(slot != nullptr))
            return;
        if (!BEAST_EXPECT(overlay.peerFinder().onConnected(slot, local)))
        {
            overlay.peerFinder().on_closed(slot);
            return;
        }
        PublicKey key(std::get<0>(randomKeyPair(KeyType::ed25519)));
        // Reserve the fixture peer independently of the standalone app's
        // automatic outbound-peer target.
        if (!BEAST_EXPECT(
                overlay.peerFinder().activate(slot, key, true) ==
                PeerFinder::Result::success))
        {
            overlay.peerFinder().on_closed(slot);
            return;
        }
        auto const disconnects = overlay.getPeerDisconnect();
        auto peer = std::make_shared<PeerImp>(
            env.app(),
            std::move(stream),
            boost::asio::const_buffer{},
            std::move(slot),
            http_response_type{},
            overlay.resourceManager().newOutboundEndpoint(endpoint),
            key,
            ProtocolVersion{1, 7},
            100000,
            overlay);
        std::weak_ptr<PeerImp> weak = peer;
        overlay.add_active(peer);
        int shutdowns = 0;
        if (graceful)
        {
            remote.async_shutdown([&](error_code) { ++shutdowns; });
        }
        else
        {
            peer->stop();
            peer->stop();
        }
        peer.reset();
        ioc.restart();
        ioc.run_for(std::chrono::seconds{5});
        if (graceful)
            BEAST_EXPECT(shutdowns == 1);
        BEAST_EXPECT(weak.expired());
        BEAST_EXPECT(overlay.size() == 0);
        BEAST_EXPECT(overlay.getPeerDisconnect() == disconnects + 1);
        BEAST_EXPECT(ioc.stopped());
        if (auto remaining = weak.lock())
            remaining->stop();
        error_code ec;
        remote.next_layer().close(ec);
        ioc.restart();
        ioc.run_for(std::chrono::seconds{1});
    }

    void
    run() override
    {
        testRefused();
        testRejected(Behavior::abortHandshake);
        testRejected(Behavior::reject);
        testStop();
        testDeadlines();
        testPeer(false);
        testPeer(true);
    }
};

BEAST_DEFINE_TESTSUITE(OverlayLifecycle, overlay, ripple);

}  // namespace test
}  // namespace ripple
