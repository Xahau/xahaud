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
#include <xrpld/net/HTTPClient.h>
#include <xrpld/net/RPCCall.h>
#include <xrpl/basics/ByteUtilities.h>

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ripple {
namespace test {

// Minimal TCP server for testing HTTPClient behavior.
// Accepts connections and sends configurable HTTP responses.
class MockHTTPServer
{
    boost::asio::io_service ios_;
    std::unique_ptr<boost::asio::io_service::work> work_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic<bool> running_{true};
    unsigned short port_;

    // Metrics
    std::atomic<int> activeConnections_{0};
    std::atomic<int> peakConnections_{0};
    std::atomic<int> totalAccepted_{0};

    // Configurable behavior
    std::atomic<int> statusCode_{200};
    std::atomic<int> delayMs_{0};
    std::atomic<bool> sendResponse_{true};
    std::atomic<bool> closeImmediately_{false};
    std::atomic<bool> noContentLength_{false};
    std::atomic<bool> partialBodyHold_{false};
    std::atomic<bool> truncatedContentLength_{false};
    std::mutex configMutex_;
    std::string responseBody_{"{}"};

    // Sockets deliberately held open (sendResponse_ == false) so the
    // client must hit its deadline. Without this the only shared_ptr to
    // the socket would drop when the accept handler returns, closing it
    // and turning a timeout test into a server-closed test.
    std::mutex heldMutex_;
    std::vector<std::shared_ptr<boost::asio::ip::tcp::socket>> heldSockets_;

public:
    MockHTTPServer()
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

    ~MockHTTPServer()
    {
        running_ = false;
        work_.reset();  // Allow io_service to stop.
        boost::system::error_code ec;
        acceptor_.close(ec);
        ios_.stop();
        if (thread_.joinable())
            thread_.join();
        // The io_service thread is joined — safe to close held sockets
        // without racing it (and no lock needed).
        for (auto& s : heldSockets_)
        {
            boost::system::error_code ig;
            s->close(ig);
        }
        heldSockets_.clear();
    }

    unsigned short
    port() const
    {
        return port_;
    }
    int
    activeConnectionCount() const
    {
        return activeConnections_;
    }
    int
    peakConnectionCount() const
    {
        return peakConnections_;
    }
    int
    totalAcceptedCount() const
    {
        return totalAccepted_;
    }

    void
    setStatus(int code)
    {
        statusCode_ = code;
    }
    void
    setDelay(int ms)
    {
        delayMs_ = ms;
    }
    void
    setSendResponse(bool send)
    {
        sendResponse_ = send;
    }
    void
    setCloseImmediately(bool close)
    {
        closeImmediately_ = close;
    }
    void
    setNoContentLength(bool noContentLength)
    {
        noContentLength_ = noContentLength;
    }
    void
    setPartialBodyHold(bool v)
    {
        partialBodyHold_ = v;
    }
    void
    setTruncatedContentLength(bool v)
    {
        truncatedContentLength_ = v;
    }
    void
    setResponseBody(std::string body)
    {
        std::lock_guard lk(configMutex_);
        responseBody_ = std::move(body);
    }

private:
    void
    accept()
    {
        auto sock = std::make_shared<boost::asio::ip::tcp::socket>(ios_);
        acceptor_.async_accept(*sock, [this, sock](auto ec) {
            if (!ec && running_)
            {
                ++totalAccepted_;
                int current = ++activeConnections_;
                int prev = peakConnections_.load();
                while (current > prev &&
                       !peakConnections_.compare_exchange_weak(prev, current))
                    ;

                handleConnection(sock);
                accept();
            }
        });
    }

    void
    handleConnection(std::shared_ptr<boost::asio::ip::tcp::socket> sock)
    {
        if (closeImmediately_)
        {
            boost::system::error_code ec;
            sock->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            sock->close(ec);
            --activeConnections_;
            return;
        }

        auto buf = std::make_shared<boost::asio::streambuf>();
        boost::asio::async_read_until(
            *sock, *buf, "\r\n\r\n", [this, sock, buf](auto ec, size_t) {
                if (ec)
                {
                    --activeConnections_;
                    return;
                }

                if (!sendResponse_)
                {
                    // Accept but never respond, simulating an overloaded
                    // endpoint. Stash the socket so it stays open past
                    // this handler — the client must time out on its own
                    // deadline rather than see the connection close.
                    std::lock_guard lk(heldMutex_);
                    heldSockets_.push_back(sock);
                    return;
                }

                if (partialBodyHold_)
                {
                    // Promise more body than we deliver, then hold the
                    // socket open. The client reads the header, blocks in
                    // handleData waiting for the rest of the body, and
                    // eventually hits its deadline — exercising the
                    // read-error completion path (mShutdown != eof).
                    auto resp = std::make_shared<std::string>(
                        "HTTP/1.0 200 OK\r\nContent-Length: 1000\r\n\r\nXX");
                    boost::asio::async_write(
                        *sock,
                        boost::asio::buffer(*resp),
                        [this, sock, resp](auto, std::size_t) {
                            std::lock_guard lk(heldMutex_);
                            heldSockets_.push_back(sock);
                        });
                    return;
                }

                auto delay = delayMs_.load();
                if (delay > 0)
                {
                    auto timer =
                        std::make_shared<boost::asio::steady_timer>(ios_);
                    timer->expires_from_now(std::chrono::milliseconds(delay));
                    timer->async_wait(
                        [this, sock, timer](auto) { sendHTTPResponse(sock); });
                }
                else
                {
                    sendHTTPResponse(sock);
                }
            });
    }

    void
    sendHTTPResponse(std::shared_ptr<boost::asio::ip::tcp::socket> sock)
    {
        auto body = [&] {
            std::lock_guard lk(configMutex_);
            return responseBody_;
        }();
        std::string header =
            "HTTP/1.0 " + std::to_string(statusCode_.load()) + " OK\r\n";
        if (truncatedContentLength_)
            header += "Content-Length: " + std::to_string(body.size() + 1000) +
                "\r\n";
        else if (!noContentLength_)
            header += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        header += "\r\n";
        auto response = std::make_shared<std::string>(header + body);

        boost::asio::async_write(
            *sock,
            boost::asio::buffer(*response),
            [this, sock, response](auto, size_t) {
                if (noContentLength_ || truncatedContentLength_)
                {
                    // EOF-delimited responses close to signal the end of the
                    // body. Truncated Content-Length responses also close here
                    // to simulate a server disappearing before the promised
                    // bytes arrive.
                    boost::system::error_code ec;
                    sock->shutdown(
                        boost::asio::ip::tcp::socket::shutdown_both, ec);
                    sock->close(ec);
                    --activeConnections_;
                    return;
                }
                // Content-Length response: keep the connection counted
                // until the CLIENT closes its end. This makes
                // activeConnectionCount() track client-side socket
                // cleanup (FD release) rather than the server's own
                // lifecycle — so a leaked HTTPClientImp shows up as a
                // connection that never drops, not a false zero.
                awaitClientClose(sock);
            });
    }

    void
    awaitClientClose(std::shared_ptr<boost::asio::ip::tcp::socket> sock)
    {
        auto drain = std::make_shared<std::array<char, 64>>();
        sock->async_read_some(
            boost::asio::buffer(*drain),
            [this, sock, drain](boost::system::error_code ec, std::size_t n) {
                if (!ec && n > 0)
                {
                    // Leftover request body or pipelined bytes — keep
                    // waiting for the client's close (EOF).
                    awaitClientClose(sock);
                    return;
                }
                boost::system::error_code ig;
                sock->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ig);
                sock->close(ig);
                --activeConnections_;
            });
    }
};

//------------------------------------------------------------------------------

class HTTPClient_test : public beast::unit_test::suite
{
    // Poll until cond() holds or the timeout elapses. The mock server now
    // decrements its connection count only when the CLIENT closes its
    // socket (it waits for the client's EOF), and that runs on the
    // server's io_service thread — so a bounded poll is more robust than
    // a fixed sleep against that cross-thread timing.
    template <class Cond>
    bool
    waitFor(
        Cond cond,
        std::chrono::milliseconds timeout = std::chrono::seconds{5})
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (!cond() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return cond();
    }

    bool
    waitForConnections(MockHTTPServer& server, int expected)
    {
        return waitFor(
            [&] { return server.activeConnectionCount() == expected; });
    }

    // Helper: fire an HTTP request and track completion via atomic counter.
    void
    fireRequest(
        boost::asio::io_service& ios,
        std::string const& host,
        unsigned short port,
        std::atomic<int>& completed,
        beast::Journal& j,
        std::chrono::seconds timeout = std::chrono::seconds{5})
    {
        HTTPClient::request(
            false,  // no SSL
            ios,
            host,
            port,
            [](boost::asio::streambuf& sb, std::string const& strHost) {
                std::ostream os(&sb);
                os << "POST / HTTP/1.0\r\n"
                   << "Host: " << strHost << "\r\n"
                   << "Content-Type: application/json\r\n"
                   << "Content-Length: 2\r\n"
                   << "\r\n"
                   << "{}";
            },
            megabytes(1),
            timeout,
            [&completed](
                const boost::system::error_code&, int, std::string const&) {
                ++completed;
                return false;
            },
            j);
    }

    //--------------------------------------------------------------------------

    void
    testCleanupAfterSuccess()
    {
        testcase("Socket cleanup after successful response");

        // After a successful HTTP request completes, the
        // HTTPClientImp should be destroyed and its socket
        // closed promptly — not held until the deadline fires.

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(200);

        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            fireRequest(ios, "127.0.0.1", server.port(), completed, j);
            ios.run();
        }

        BEAST_EXPECT(completed == 1);
        BEAST_EXPECT(server.totalAcceptedCount() == 1);
        // After io_service.run() returns, the server should
        // see zero active connections — socket was released.
        BEAST_EXPECT(waitForConnections(server, 0));
    }

    void
    testCleanupAfter500()
    {
        testcase("Socket cleanup after HTTP 500");

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(500);

        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            fireRequest(ios, "127.0.0.1", server.port(), completed, j);
            ios.run();
        }

        BEAST_EXPECT(completed == 1);
        BEAST_EXPECT(waitForConnections(server, 0));
    }

    void
    testCleanupAfterConnectionRefused()
    {
        testcase("Socket cleanup after connection refused");

        using namespace jtx;
        Env env{*this};

        // Bind a port, then close it — guarantees nothing is listening.
        boost::asio::io_service tmp;
        boost::asio::ip::tcp::acceptor acc(
            tmp,
            boost::asio::ip::tcp::endpoint(
                boost::asio::ip::address::from_string("127.0.0.1"), 0));
        auto port = acc.local_endpoint().port();
        acc.close();

        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            fireRequest(ios, "127.0.0.1", port, completed, j);
            ios.run();
        }

        // Callback should still be invoked (with error).
        BEAST_EXPECT(completed == 1);
    }

    void
    testCleanupAfterTimeout()
    {
        testcase("Socket cleanup after timeout");

        // Server accepts but never responds. HTTPClient should
        // time out, clean up, and invoke the callback.

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setSendResponse(false);  // accept, read, but never respond

        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            // Short timeout to keep the test fast.
            fireRequest(
                ios,
                "127.0.0.1",
                server.port(),
                completed,
                j,
                std::chrono::seconds{2});
            ios.run();
        }

        // Callback must be invoked even on timeout.
        BEAST_EXPECT(completed == 1);
    }

    void
    testReadErrorDuringBody()
    {
        testcase("Read error during body invokes callback");

        // Server sends a header promising 1000 body bytes but delivers
        // only 2, then holds the socket. The client blocks in handleData
        // waiting for the rest and hits its deadline, driving the
        // read-error completion path (mShutdown != eof). The callback
        // must still fire.

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setPartialBodyHold(true);

        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            fireRequest(
                ios,
                "127.0.0.1",
                server.port(),
                completed,
                j,
                std::chrono::seconds{2});
            ios.run();
        }

        BEAST_EXPECT(completed == 1);
    }

    void
    testCleanupAfterServerCloseBeforeResponse()
    {
        testcase("Socket cleanup after server closes before response");

        // Server accepts the connection then immediately closes
        // it without sending anything.

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setCloseImmediately(true);

        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            fireRequest(ios, "127.0.0.1", server.port(), completed, j);
            ios.run();
        }

        BEAST_EXPECT(completed == 1);
        BEAST_EXPECT(waitForConnections(server, 0));
    }

    void
    testEOFCompletionCallsCallback()
    {
        testcase("EOF completion invokes callback (handleData bug)");

        // HTTPClientImp::handleData has a code path where
        // mShutdown == eof results in logging "Complete." but
        // never calling invokeComplete(). This means:
        //   - The completion callback is never invoked
        //   - The deadline timer is never cancelled
        //   - The socket is held open until the 30s deadline
        //
        // This test verifies the callback IS invoked after an
        // EOF response. If this test fails (completed == 0 after
        // ios.run()), the handleData EOF bug is confirmed.

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(200);
        // Without this the mock sends Content-Length and completion
        // happens in handleHeader, NOT the handleData EOF branch this
        // test targets. Force the EOF (no-Content-Length) path.
        server.setNoContentLength(true);

        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            fireRequest(
                ios,
                "127.0.0.1",
                server.port(),
                completed,
                j,
                std::chrono::seconds{3});
            ios.run();
        }

        // If handleData EOF path doesn't call invokeComplete,
        // the callback won't fire until the deadline (3s) expires,
        // and even then handleDeadline doesn't invoke mComplete.
        // The io_service.run() will still return (deadline fires,
        // handleShutdown runs, all handlers done), but completed
        // will be 0.
        if (completed != 1)
        {
            log << "  BUG CONFIRMED: handleData EOF path does not"
                << " call invokeComplete(). Callback was not invoked."
                << " Socket held open until deadline." << std::endl;
        }
        BEAST_EXPECT(completed == 1);
    }

    void
    testConcurrentRequestCleanup()
    {
        testcase("Concurrent requests all clean up");

        // Fire N requests at once on the same io_service.
        // All should complete and release their sockets.

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(200);

        static constexpr int N = 50;
        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            for (int i = 0; i < N; ++i)
            {
                fireRequest(ios, "127.0.0.1", server.port(), completed, j);
            }
            ios.run();
        }

        BEAST_EXPECT(completed == N);
        // Brief sleep to let server-side shutdown complete.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        BEAST_EXPECT(waitForConnections(server, 0));

        log << "  Completed: " << completed
            << ", Peak concurrent: " << server.peakConnectionCount()
            << ", Active after: " << server.activeConnectionCount()
            << std::endl;
    }

    void
    testConcurrent500Cleanup()
    {
        testcase("Concurrent 500 requests all clean up");

        // Fire N requests that all get 500 responses. Verify
        // all sockets are released and no FDs leak.

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(500);

        static constexpr int N = 50;
        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            for (int i = 0; i < N; ++i)
            {
                fireRequest(ios, "127.0.0.1", server.port(), completed, j);
            }
            ios.run();
        }

        BEAST_EXPECT(completed == N);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        BEAST_EXPECT(waitForConnections(server, 0));
    }

    void
    testEOFWithoutContentLength()
    {
        testcase("EOF without Content-Length (handleData EOF path)");

        // When a server sends a response WITHOUT Content-Length,
        // HTTPClientImp reads up to maxResponseSize. The server
        // closes the connection, causing EOF in handleData.
        //
        // In handleData, the EOF path (mShutdown == eof) logs
        // "Complete." but does NOT call invokeComplete(). This
        // means:
        //   - mComplete (callback) is never invoked
        //   - deadline timer is never cancelled
        //   - socket + object held alive until deadline fires
        //
        // This test uses a SHORT deadline to keep it fast. If
        // the callback IS invoked, ios.run() returns quickly.
        // If NOT, ios.run() blocks until the deadline (2s).

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(200);
        server.setNoContentLength(true);

        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        auto start = std::chrono::steady_clock::now();
        {
            boost::asio::io_service ios;
            fireRequest(
                ios,
                "127.0.0.1",
                server.port(),
                completed,
                j,
                std::chrono::seconds{2});
            ios.run();
        }
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                      .count();

        if (completed == 0)
        {
            log << "  BUG CONFIRMED: handleData EOF path does not"
                << " call invokeComplete(). Callback never invoked."
                << " io_service.run() blocked for " << ms << "ms"
                << " (deadline timeout)." << std::endl;
        }
        else
        {
            log << "  Callback invoked in " << ms << "ms." << std::endl;
        }
        // This WILL fail if the EOF bug exists — the callback
        // is only invoked via the deadline timeout path, which
        // does NOT call mComplete.
        BEAST_EXPECT(completed == 1);
    }

    void
    testEOFWithoutContentLengthIsSuccess()
    {
        testcase("EOF without Content-Length reports success");

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(200);
        server.setNoContentLength(true);

        std::atomic<int> completed{0};
        std::atomic<bool> sawError{true};
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            HTTPClient::request(
                false,
                ios,
                "127.0.0.1",
                server.port(),
                [](boost::asio::streambuf& sb, std::string const& strHost) {
                    std::ostream os(&sb);
                    os << "POST / HTTP/1.0\r\n"
                       << "Host: " << strHost << "\r\n"
                       << "Content-Type: application/json\r\n"
                       << "Content-Length: 2\r\n"
                       << "\r\n"
                       << "{}";
                },
                megabytes(1),
                std::chrono::seconds{2},
                [&completed, &sawError](
                    const boost::system::error_code& ecResult,
                    int,
                    std::string const&) {
                    sawError = static_cast<bool>(ecResult);
                    ++completed;
                    return false;
                },
                j);
            ios.run();
        }

        BEAST_EXPECT(completed == 1);
        BEAST_EXPECT(!sawError);
    }

    void
    testTruncatedContentLengthIsError()
    {
        testcase("Truncated Content-Length reports error");

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(200);
        server.setTruncatedContentLength(true);

        std::atomic<int> completed{0};
        std::atomic<bool> sawError{false};
        std::string data;
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            HTTPClient::request(
                false,
                ios,
                "127.0.0.1",
                server.port(),
                [](boost::asio::streambuf& sb, std::string const& strHost) {
                    std::ostream os(&sb);
                    os << "POST / HTTP/1.0\r\n"
                       << "Host: " << strHost << "\r\n"
                       << "Content-Type: application/json\r\n"
                       << "Content-Length: 2\r\n"
                       << "\r\n"
                       << "{}";
                },
                megabytes(1),
                std::chrono::seconds{2},
                [&completed, &sawError, &data](
                    const boost::system::error_code& ecResult,
                    int,
                    std::string const& strData) {
                    sawError = static_cast<bool>(ecResult);
                    data = strData;
                    ++completed;
                    return false;
                },
                j);
            ios.run();
        }

        BEAST_EXPECT(completed == 1);
        BEAST_EXPECT(sawError);
        BEAST_EXPECT(data == "{}");
    }

    void
    testRPCCallAcceptsCompleteContentLength()
    {
        testcase("RPCCall accepts complete Content-Length");

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(200);
        server.setResponseBody(R"({"status":"success"})");

        std::atomic<int> callbackCount{0};
        bool threw = false;
        bool sawResult = false;

        try
        {
            boost::asio::io_service ios;
            Json::Value params(Json::arrayValue);
            RPCCall::fromNetwork(
                ios,
                "127.0.0.1",
                server.port(),
                "",
                "",
                "",
                "server_info",
                params,
                false,
                true,
                env.app().logs(),
                [&callbackCount, &sawResult](Json::Value const& jvInput) {
                    ++callbackCount;
                    sawResult = jvInput.isMember("result") &&
                        jvInput["result"].isObject();
                });
            ios.run();
        }
        catch (std::exception const& e)
        {
            threw = true;
            log << "  unexpected RPCCall exception: " << e.what() << std::endl;
        }

        BEAST_EXPECT(!threw);
        BEAST_EXPECT(callbackCount == 1);
        BEAST_EXPECT(sawResult);
    }

    void
    testRPCCallRejectsTruncatedContentLength()
    {
        testcase("RPCCall rejects truncated Content-Length");

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(200);
        server.setTruncatedContentLength(true);

        std::atomic<int> callbackCount{0};
        bool threw = false;

        try
        {
            boost::asio::io_service ios;
            Json::Value params(Json::arrayValue);
            RPCCall::fromNetwork(
                ios,
                "127.0.0.1",
                server.port(),
                "",
                "",
                "",
                "server_info",
                params,
                false,
                true,
                env.app().logs(),
                [&callbackCount](Json::Value const&) { ++callbackCount; });
            ios.run();
        }
        catch (std::exception const& e)
        {
            threw = true;
            log << "  RPCCall exception: " << e.what() << std::endl;
        }

        BEAST_EXPECT(threw);
        BEAST_EXPECT(callbackCount == 0);
    }

    void
    testPersistentIOServiceCleanup()
    {
        testcase("Cleanup on persistent io_service (no destructor mask)");

        // Previous tests destroy the io_service after run(),
        // which releases all pending handlers' shared_ptrs.
        // This masks leaks. Here we use a PERSISTENT io_service
        // (with work guard, running on its own thread) and check
        // that HTTPClientImp objects are destroyed WITHOUT relying
        // on io_service destruction.
        //
        // We track the object's lifetime via the completion
        // callback — if it fires, the async chain completed
        // normally. If it doesn't fire within a reasonable time
        // but the io_service is still running, something is stuck.

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(200);

        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        // Persistent io_service — stays alive the whole test.
        boost::asio::io_service ios;
        auto work = std::make_unique<boost::asio::io_service::work>(ios);
        std::thread runner([&ios] { ios.run(); });

        // Fire request on the persistent io_service.
        HTTPClient::request(
            false,
            ios,
            "127.0.0.1",
            server.port(),
            [](boost::asio::streambuf& sb, std::string const& strHost) {
                std::ostream os(&sb);
                os << "POST / HTTP/1.0\r\n"
                   << "Host: " << strHost << "\r\n"
                   << "Content-Type: application/json\r\n"
                   << "Content-Length: 2\r\n"
                   << "\r\n"
                   << "{}";
            },
            megabytes(1),
            std::chrono::seconds{5},
            [&completed](
                const boost::system::error_code&, int, std::string const&) {
                ++completed;
                return false;
            },
            j);

        // Wait for completion without destroying io_service.
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (completed == 0 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        BEAST_EXPECT(completed == 1);

        // Give server-side shutdown a moment.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        BEAST_EXPECT(waitForConnections(server, 0));

        if (server.activeConnectionCount() != 0)
        {
            log << "  BUG: Socket still open on persistent"
                << " io_service. FD leaked." << std::endl;
        }

        // Clean shutdown.
        work.reset();
        ios.stop();
        runner.join();
    }

    void
    testPersistentIOService500Cleanup()
    {
        testcase("500 cleanup on persistent io_service");

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(500);

        static constexpr int N = 20;
        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        boost::asio::io_service ios;
        auto work = std::make_unique<boost::asio::io_service::work>(ios);
        std::thread runner([&ios] { ios.run(); });

        for (int i = 0; i < N; ++i)
        {
            HTTPClient::request(
                false,
                ios,
                "127.0.0.1",
                server.port(),
                [](boost::asio::streambuf& sb, std::string const& strHost) {
                    std::ostream os(&sb);
                    os << "POST / HTTP/1.0\r\n"
                       << "Host: " << strHost << "\r\n"
                       << "Content-Type: application/json\r\n"
                       << "Content-Length: 2\r\n"
                       << "\r\n"
                       << "{}";
                },
                megabytes(1),
                std::chrono::seconds{5},
                [&completed](
                    const boost::system::error_code&, int, std::string const&) {
                    ++completed;
                    return false;
                },
                j);
        }

        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (completed < N && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        BEAST_EXPECT(completed == N);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        BEAST_EXPECT(waitForConnections(server, 0));

        log << "  Completed: " << completed << "/" << N
            << ", Active connections after: " << server.activeConnectionCount()
            << std::endl;

        work.reset();
        ios.stop();
        runner.join();
    }

    void
    testGetSelfReferenceCleanup()
    {
        testcase("get() shared_from_this cycle releases");

        // HTTPClientImp::get() binds shared_from_this() into
        // mBuild via makeGet. This creates a reference cycle:
        //   object -> mBuild -> shared_ptr<object>
        // The object can only be destroyed if mBuild is cleared.
        // Since mBuild is never explicitly cleared, this may be
        // a permanent FD leak.
        //
        // This test fires a GET request and checks whether the
        // HTTPClientImp is destroyed (and socket closed) after
        // completion.

        using namespace jtx;
        Env env{*this};

        MockHTTPServer server;
        server.setStatus(200);

        std::atomic<int> completed{0};
        auto j = env.app().journal("HTTPClient");

        {
            boost::asio::io_service ios;
            HTTPClient::get(
                false,  // no SSL
                ios,
                "127.0.0.1",
                server.port(),
                "/test",
                megabytes(1),
                std::chrono::seconds{5},
                [&completed](
                    const boost::system::error_code&, int, std::string const&) {
                    ++completed;
                    return false;
                },
                j);
            ios.run();
        }

        BEAST_EXPECT(completed == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // If the get() self-reference cycle leaks, the server
        // will still show an active connection here (the socket
        // in the leaked HTTPClientImp is never closed).
        if (server.activeConnectionCount() != 0)
        {
            log << "  BUG CONFIRMED: get() self-reference cycle"
                << " prevents HTTPClientImp destruction."
                << " Socket FD leaked." << std::endl;
        }
        BEAST_EXPECT(waitForConnections(server, 0));
    }

public:
    void
    run() override
    {
        testCleanupAfterSuccess();
        testCleanupAfter500();
        testCleanupAfterConnectionRefused();
        testCleanupAfterTimeout();
        testReadErrorDuringBody();
        testCleanupAfterServerCloseBeforeResponse();
        testEOFCompletionCallsCallback();
        testConcurrentRequestCleanup();
        testConcurrent500Cleanup();
        testEOFWithoutContentLength();
        testEOFWithoutContentLengthIsSuccess();
        testTruncatedContentLengthIsError();
        testRPCCallAcceptsCompleteContentLength();
        testRPCCallRejectsTruncatedContentLength();
        testPersistentIOServiceCleanup();
        testPersistentIOService500Cleanup();
        testGetSelfReferenceCleanup();
    }
};

BEAST_DEFINE_TESTSUITE(HTTPClient, net, ripple);

}  // namespace test
}  // namespace ripple
