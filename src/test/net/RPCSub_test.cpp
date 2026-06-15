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
#include <xrpld/core/Job.h>
#include <xrpld/core/JobQueue.h>
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
    std::atomic<int> delayMs_{0};

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

    // Delay each reply so delivery is deterministically slower than the
    // microsecond-fast enqueue loop — keeps the deque full for the
    // queue-cap drop test regardless of scheduling.
    void
    setResponseDelay(int ms)
    {
        delayMs_ = ms;
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

                auto const delay = delayMs_.load();
                if (delay > 0)
                {
                    auto timer =
                        std::make_shared<boost::asio::steady_timer>(ios_);
                    timer->expires_from_now(std::chrono::milliseconds(delay));
                    timer->async_wait(
                        [this, sock, timer](auto) { reply(sock); });
                }
                else
                {
                    reply(sock);
                }
            });
    }

    void
    reply(std::shared_ptr<boost::asio::ip::tcp::socket> sock)
    {
        // EOF-delimited reply: no Content-Length, close after writing.
        // This is the realistic failing-webhook shape.
        auto resp = std::make_shared<std::string>(
            "HTTP/1.0 " + std::to_string(status_.load()) +
            " Reply\r\n\r\n{\"result\":{}}");
        boost::asio::async_write(
            *sock, boost::asio::buffer(*resp), [sock, resp](auto, std::size_t) {
                boost::system::error_code ig;
                sock->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ig);
                sock->close(ig);
            });
    }
};

//------------------------------------------------------------------------------

class RPCSub_test : public beast::unit_test::suite
{
    // Generous ceiling: the instrumented Debug (coverage) build is much
    // slower than Release, so timeouts are sized for that, not Release.
    template <class Cond>
    bool
    waitFor(Cond cond, std::chrono::seconds timeout = std::chrono::seconds{30})
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (!cond() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return cond();
    }

    std::shared_ptr<RPCSub>
    makeSub(
        jtx::Env& env,
        MockWebhookEndpoint& ep,
        std::size_t maxQueueSize = 16384)
    {
        return make_RPCSub(
            env.app().getOPs(),
            env.app().getJobQueue(),
            "http://127.0.0.1:" + std::to_string(ep.port()) + "/",
            "",
            "",
            env.app().logs(),
            maxQueueSize);
    }

    // True once no RPCSub sending job is queued or running. sendThread
    // captures a raw `this`, so the RPCSub must not be destroyed while a
    // job is still in flight — wait on this before letting the sub die.
    bool
    sendingIdle(jtx::Env& env)
    {
        return env.app().getJobQueue().getJobCountTotal(jtCLIENT_SUBSCRIBE) ==
            0;
    }

    // Wait for all events to reach the endpoint AND the sending job to
    // finish, so the sub can be torn down without racing sendThread.
    void
    drainAndSettle(jtx::Env& env, MockWebhookEndpoint& ep, int expected)
    {
        bool const delivered =
            waitFor([&] { return ep.received() >= expected; });
        bool const idle = waitFor([&] { return sendingIdle(env); });
        log << "  drainAndSettle: received=" << ep.received() << "/" << expected
            << " idle=" << idle << std::endl;
        BEAST_EXPECT(delivered);
        BEAST_EXPECT(idle);
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

        using namespace jtx;
        Env env{*this};
        MockWebhookEndpoint ep;

        static constexpr int N = 10;
        {
            auto sub = makeSub(env, ep);
            for (int i = 0; i < N; ++i)
                send(sub, i);
            drainAndSettle(env, ep, N);
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

        static constexpr int N = 10;
        {
            auto sub = makeSub(env, ep);
            for (int i = 0; i < N; ++i)
                send(sub, i);
            drainAndSettle(env, ep, N);
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

            // First burst, then wait for the sending job to fully drain
            // and exit (mSending cleared) — deterministically, not via a
            // sleep.
            for (int i = 0; i < 5; ++i)
                send(sub, i);
            drainAndSettle(env, ep, 5);

            // Second burst must start a fresh sending job.
            for (int i = 5; i < 10; ++i)
                send(sub, i);
            drainAndSettle(env, ep, 10);
        }

        BEAST_EXPECT(ep.received() == 10);
    }

    void
    testQueueCapDrops()
    {
        testcase("Events past the queue cap are dropped");

        // With a tiny cap, pushing far more events than delivery can keep
        // up with forces send() down the drop path: enqueue is microsecond
        // -fast while each (delayed) HTTP delivery is a full round-trip, so
        // the deque sits at the cap and excess events are dropped. The
        // delay makes "delivery slower than enqueue" hold regardless of
        // scheduling, so this isn't timing-dependent. We just need some
        // delivered (cap works) and some dropped (drop path exercised).
        using namespace jtx;
        Env env{*this};
        MockWebhookEndpoint ep;
        ep.setResponseDelay(50);

        static constexpr int pushed = 50;
        {
            auto sub = makeSub(env, ep, /*maxQueueSize*/ 2);
            for (int i = 0; i < pushed; ++i)
                send(sub, i);
            BEAST_EXPECT(waitFor([&] { return sendingIdle(env); }));
        }

        log << "  queue cap: received " << ep.received() << "/" << pushed
            << std::endl;
        BEAST_EXPECT(ep.received() > 0);
        BEAST_EXPECT(ep.received() < pushed);
    }

public:
    void
    run() override
    {
        testDelivery();
        testErrorsDoNotStall();
        testRestartAfterDrain();
        testQueueCapDrops();
    }
};

BEAST_DEFINE_TESTSUITE(RPCSub, net, ripple);

}  // namespace test
}  // namespace ripple
