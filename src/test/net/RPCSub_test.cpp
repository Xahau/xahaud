//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 Ripple Labs Inc.

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
#include <xrpld/net/RPCSub.h>
#include <xrpl/json/json_value.h>

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace ripple {
namespace test {

// Minimal HTTP endpoint that counts received webhook POSTs and replies
// with a configurable status. Responses are EOF-delimited (no
// Content-Length) and the socket is closed right after writing — the
// exact shape that triggered the original handleData EOF-completion
// leak. So these tests exercise RPCSub flow control AND the HTTPClient
// EOF fix end to end: if either regressed, delivery would stall and the
// expected count would never be reached within the timeout.
class MockWebhookEndpoint
{
    boost::asio::io_service ios_;
    std::unique_ptr<boost::asio::io_service::work> work_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
    unsigned short port_;

    std::atomic<int> received_{0};
    std::atomic<int> status_{200};

public:
    MockWebhookEndpoint()
        : work_(std::make_unique<boost::asio::io_service::work>(ios_))
        , acceptor_(
              ios_,
              boost::asio::ip::tcp::endpoint(
                  boost::asio::ip::address::from_string("127.0.0.1"),
                  0))
    {
        port_ = acceptor_.local_endpoint().port();
        accept();
        thread_ = std::thread([this] { ios_.run(); });
    }

    ~MockWebhookEndpoint()
    {
        work_.reset();
        boost::system::error_code ec;
        acceptor_.close(ec);
        ios_.stop();
        if (thread_.joinable())
            thread_.join();
    }

    unsigned short
    port() const
    {
        return port_;
    }

    int
    received() const
    {
        return received_;
    }

    void
    setStatus(int s)
    {
        status_ = s;
    }

private:
    void
    accept()
    {
        auto sock = std::make_shared<boost::asio::ip::tcp::socket>(ios_);
        acceptor_.async_accept(*sock, [this, sock](auto ec) {
            if (ec)
                return;
            handle(sock);
            accept();
        });
    }

    void
    handle(std::shared_ptr<boost::asio::ip::tcp::socket> sock)
    {
        auto buf = std::make_shared<boost::asio::streambuf>();
        boost::asio::async_read_until(
            *sock, *buf, "\r\n\r\n", [this, sock, buf](auto ec, std::size_t) {
                if (ec)
                    return;

                ++received_;

                // EOF-delimited reply: no Content-Length, close after
                // writing. This is the realistic failing-webhook shape.
                auto resp = std::make_shared<std::string>(
                    "HTTP/1.0 " + std::to_string(status_.load()) +
                    " Reply\r\n\r\n{\"result\":{}}");
                boost::asio::async_write(
                    *sock,
                    boost::asio::buffer(*resp),
                    [sock, resp](auto, std::size_t) {
                        boost::system::error_code ig;
                        sock->shutdown(
                            boost::asio::ip::tcp::socket::shutdown_both, ig);
                        sock->close(ig);
                    });
            });
    }
};

//------------------------------------------------------------------------------

class RPCSub_test : public beast::unit_test::suite
{
    template <class Cond>
    bool
    waitFor(
        Cond cond,
        std::chrono::milliseconds timeout = std::chrono::seconds{10})
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (!cond() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return cond();
    }

    std::shared_ptr<RPCSub>
    makeSub(jtx::Env& env, MockWebhookEndpoint& ep)
    {
        return make_RPCSub(
            env.app().getOPs(),
            env.app().getJobQueue(),
            "http://127.0.0.1:" + std::to_string(ep.port()) + "/",
            "",
            "",
            env.app().logs());
    }

    // Wait until all queued events have been delivered, then give the
    // sending job a moment to finish. sendThread captures a raw `this`,
    // so the RPCSub must not be destroyed while it is still running.
    void
    drainAndSettle(MockWebhookEndpoint& ep, int expected)
    {
        BEAST_EXPECT(waitFor([&] { return ep.received() >= expected; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    void
    send(std::shared_ptr<RPCSub> const& sub, int n)
    {
        Json::Value ev(Json::objectValue);
        ev["n"] = n;
        sub->send(ev, false);
    }

    void
    testDelivery()
    {
        testcase("Webhook events are delivered");

        // N > maxInFlight(32) so sendThread drains in multiple batches
        // within a single invocation (exercises the dispatch loop).
        using namespace jtx;
        Env env{*this};
        MockWebhookEndpoint ep;

        static constexpr int N = 50;
        {
            auto sub = makeSub(env, ep);
            for (int i = 0; i < N; ++i)
                send(sub, i);
            drainAndSettle(ep, N);
        }

        BEAST_EXPECT(ep.received() == N);
    }

    void
    testErrorsDoNotStall()
    {
        testcase("Delivery continues when endpoint returns HTTP 500");

        // The original bug (xrpld #6341): an endpoint returning errors
        // without Content-Length never completed, stalling delivery to
        // ALL subscribers. Here every response is a 500 with no
        // Content-Length (EOF-delimited) — all N must still arrive.
        using namespace jtx;
        Env env{*this};
        MockWebhookEndpoint ep;
        ep.setStatus(500);

        static constexpr int N = 50;
        {
            auto sub = makeSub(env, ep);
            for (int i = 0; i < N; ++i)
                send(sub, i);
            drainAndSettle(ep, N);
        }

        BEAST_EXPECT(ep.received() == N);
    }

    void
    testRestartAfterDrain()
    {
        testcase("Sending restarts after the queue drains");

        // After a batch drains, sendThread clears mSending and returns.
        // A later send() must start a fresh sending job; if mSending were
        // left set (the #6341 failure mode) the second burst would never
        // be delivered.
        using namespace jtx;
        Env env{*this};
        MockWebhookEndpoint ep;

        {
            auto sub = makeSub(env, ep);

            for (int i = 0; i < 20; ++i)
                send(sub, i);
            BEAST_EXPECT(waitFor([&] { return ep.received() >= 20; }));
            // Let the first sending job finish so mSending goes false.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            for (int i = 20; i < 40; ++i)
                send(sub, i);
            drainAndSettle(ep, 40);
        }

        BEAST_EXPECT(ep.received() == 40);
    }

public:
    void
    run() override
    {
        testDelivery();
        testErrorsDoNotStall();
        testRestartAfterDrain();
    }
};

BEAST_DEFINE_TESTSUITE(RPCSub, net, ripple);

}  // namespace test
}  // namespace ripple
