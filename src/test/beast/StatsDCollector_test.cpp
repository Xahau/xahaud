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

#include <test/unit_test/SuiteJournal.h>

#include <xrpl/beast/insight/StatsDCollector.h>
#include <xrpl/beast/unit_test.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ripple {
namespace test {

// The StatsD collector was ported from io_service / strand.wrap /
// expires_from_now to io_context / bind_executor / expires_after for Boost
// 1.87+. Nothing exercised it before; this drives every path of the port:
// metric posts (dispatch onto the strand), the 1 s flush tick, the UDP send,
// and the timer-cancelling teardown.
class StatsDCollector_test : public beast::unit_test::suite
{
    // A StatsD "server": a loopback UDP socket polled by its own thread.
    struct UdpSink
    {
        boost::asio::io_context ioc;
        boost::asio::ip::udp::socket socket;
        std::mutex mutex;
        std::string received;
        std::atomic<bool> stopping{false};
        std::thread thread;

        UdpSink()
            : socket(
                  ioc,
                  boost::asio::ip::udp::endpoint{
                      boost::asio::ip::make_address("127.0.0.1"),
                      0})
        {
            socket.non_blocking(true);
            thread = std::thread([this] { poll(); });
        }

        ~UdpSink()
        {
            stopping = true;
            thread.join();
        }

        beast::IP::Endpoint
        endpoint() const
        {
            auto const local = socket.local_endpoint();
            // IP::Address is boost::asio::ip::address itself
            return beast::IP::Endpoint{local.address(), local.port()};
        }

        void
        poll()
        {
            char buf[2048];
            while (!stopping)
            {
                boost::system::error_code ec;
                auto const n = socket.receive(boost::asio::buffer(buf), 0, ec);
                if (!ec && n > 0)
                {
                    std::lock_guard lock(mutex);
                    received.append(buf, n);
                    received.push_back('\n');
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
        }

        template <class Cond>
        bool
        waitFor(Cond cond, std::chrono::milliseconds timeout)
        {
            auto const deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                {
                    std::lock_guard lock(mutex);
                    if (cond(received))
                        return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            std::lock_guard lock(mutex);
            return cond(received);
        }
    };

    static bool
    has(std::string const& all, std::string const& s)
    {
        return all.find(s) != std::string::npos;
    }

    void
    testMetricsReachServer()
    {
        testcase("counters, gauges, meters, events and hooks reach the server");
        using namespace std::chrono_literals;

        UdpSink sink;
        SuiteJournal journal("StatsDCollector_test", *this);
        std::atomic<int> hookCalls{0};
        {
            auto collector = beast::insight::StatsDCollector::New(
                sink.endpoint(), "prefix", journal);

            auto hook = collector->make_hook([&] { ++hookCalls; });
            auto counter = collector->make_counter("hits");
            auto gauge = collector->make_gauge("depth");
            auto meter = collector->make_meter("rate");
            auto event = collector->make_event("latency");

            counter.increment(5);
            gauge.set(42);
            meter.increment(3);
            event.notify(7ms);

            // counters/gauges/meters flush on the collector's 1 s tick and
            // the batch leaves in one or more datagrams; events post at once
            // but travel with the same batch
            bool const arrived = sink.waitFor(
                [](std::string const& all) {
                    return has(all, "prefix.hits:5|c") &&
                        has(all, "prefix.depth:42|g") &&
                        has(all, "prefix.rate:3|") &&
                        has(all, "prefix.latency:7|ms");
                },
                5000ms);
            BEAST_EXPECT(arrived);
            if (!arrived)
            {
                std::lock_guard lock(sink.mutex);
                log << "received: " << sink.received << std::endl;
            }
        }
        // leaving the scope destroys the collector: it cancels its flush
        // timer, drops its work guard and joins its io thread
        BEAST_EXPECT(hookCalls >= 1);
    }

    void
    testTeardownWithoutTraffic()
    {
        testcase("a collector with no metrics tears down promptly");
        UdpSink sink;
        SuiteJournal journal("StatsDCollector_test", *this);
        auto const start = std::chrono::steady_clock::now();
        {
            auto collector = beast::insight::StatsDCollector::New(
                sink.endpoint(), "idle", journal);
            (void)collector;
        }
        auto const elapsed = std::chrono::steady_clock::now() - start;
        // the destructor must not wait for the next 1 s tick
        BEAST_EXPECT(elapsed < std::chrono::seconds(3));
    }

public:
    void
    run() override
    {
        testMetricsReachServer();
        testTeardownWithoutTraffic();
    }
};

BEAST_DEFINE_TESTSUITE(StatsDCollector, insight, ripple);

}  // namespace test
}  // namespace ripple
