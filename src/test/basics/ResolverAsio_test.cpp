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

#include <xrpl/basics/ResolverAsio.h>
#include <xrpl/beast/unit_test.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ripple {
namespace test {

// ResolverAsio was ported from io_service / strand.wrap / resolver iterators
// to io_context / bind_executor / results_type for Boost 1.87+. This covers
// the ported dispatch, resolve, results iteration, work chaining and stop
// paths against a live io_context on a worker thread.
class ResolverAsio_test : public beast::unit_test::suite
{
    struct Loop
    {
        boost::asio::io_context ioc;
        std::optional<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>>
            work{boost::asio::make_work_guard(ioc)};
        std::thread thread{[this] { ioc.run(); }};

        ~Loop()
        {
            work.reset();
            ioc.stop();
            thread.join();
        }
    };

    using Result = std::pair<std::string, std::vector<beast::IP::Endpoint>>;

    struct Results
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<Result> items;

        Resolver::HandlerType
        handler()
        {
            return [this](
                       std::string name,
                       std::vector<beast::IP::Endpoint> endpoints) {
                std::lock_guard lock(mutex);
                items.emplace_back(std::move(name), std::move(endpoints));
                cv.notify_all();
            };
        }

        bool
        waitFor(std::size_t n, std::chrono::milliseconds timeout)
        {
            std::unique_lock lock(mutex);
            return cv.wait_for(
                lock, timeout, [&] { return items.size() >= n; });
        }
    };

    void
    testNumericHost()
    {
        testcase("a numeric host:port resolves to itself");
        using namespace std::chrono_literals;
        Loop loop;
        SuiteJournal journal("ResolverAsio_test", *this);
        auto resolver = ResolverAsio::New(loop.ioc, journal);
        resolver->start();

        Results results;
        resolver->resolve({"127.0.0.1:2222"}, results.handler());
        BEAST_EXPECT(results.waitFor(1, 5000ms));
        {
            std::lock_guard lock(results.mutex);
            if (BEAST_EXPECT(results.items.size() == 1))
            {
                auto const& [name, endpoints] = results.items.front();
                BEAST_EXPECT(name == "127.0.0.1:2222");
                if (BEAST_EXPECT(endpoints.size() == 1))
                {
                    BEAST_EXPECT(endpoints.front().port() == 2222);
                    BEAST_EXPECT(
                        endpoints.front().address().to_string() == "127.0.0.1");
                }
            }
        }
        resolver->stop();
    }

    void
    testBatchSkipsUnparseable()
    {
        testcase("a batch resolves in order; unparseable names are skipped");
        using namespace std::chrono_literals;
        Loop loop;
        SuiteJournal journal("ResolverAsio_test", *this);
        auto resolver = ResolverAsio::New(loop.ioc, journal);
        resolver->start();

        Results results;
        // the whitespace-only name has no host: the resolver logs and moves
        // on without invoking the handler for it
        resolver->resolve(
            {"127.0.0.1:1", "   ", "127.0.0.1:2", "127.0.0.1:3"},
            results.handler());
        BEAST_EXPECT(results.waitFor(3, 5000ms));
        {
            std::lock_guard lock(results.mutex);
            if (BEAST_EXPECT(results.items.size() == 3))
            {
                BEAST_EXPECT(results.items[0].first == "127.0.0.1:1");
                BEAST_EXPECT(results.items[1].first == "127.0.0.1:2");
                BEAST_EXPECT(results.items[2].first == "127.0.0.1:3");
                for (auto const& [name, endpoints] : results.items)
                    BEAST_EXPECT(endpoints.size() == 1);
            }
        }
        resolver->stop();
    }

    void
    testUnresolvableName()
    {
        testcase("an unresolvable name reports no endpoints");
        using namespace std::chrono_literals;
        Loop loop;
        SuiteJournal journal("ResolverAsio_test", *this);
        auto resolver = ResolverAsio::New(loop.ioc, journal);
        resolver->start();

        Results results;
        // .invalid is reserved to never resolve (RFC 2606)
        resolver->resolve({"nonexistent.invalid:80"}, results.handler());
        // a DNS error surfaces as a callback with an empty list; a slow or
        // absent resolver makes this take a few seconds, not forever
        BEAST_EXPECT(results.waitFor(1, 20000ms));
        {
            std::lock_guard lock(results.mutex);
            if (BEAST_EXPECT(results.items.size() == 1))
            {
                BEAST_EXPECT(
                    results.items.front().first == "nonexistent.invalid:80");
                BEAST_EXPECT(results.items.front().second.empty());
            }
        }
        resolver->stop();
    }

    void
    testStopWithPendingWork()
    {
        testcase("stop with work pending returns and drops the queue");
        using namespace std::chrono_literals;
        Loop loop;
        SuiteJournal journal("ResolverAsio_test", *this);
        auto resolver = ResolverAsio::New(loop.ioc, journal);
        resolver->start();

        Results results;
        std::vector<std::string> names;
        for (int i = 1; i <= 200; ++i)
            names.push_back("127.0.0.1:" + std::to_string(i));
        resolver->resolve(names, results.handler());
        // a synchronous stop waits for the in-flight handler and clears the
        // rest; it must return promptly and the destructor's invariants
        // (no pending work, stopped) must then hold
        auto const start = std::chrono::steady_clock::now();
        resolver->stop();
        BEAST_EXPECT(
            std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
        std::size_t delivered;
        {
            std::lock_guard lock(results.mutex);
            delivered = results.items.size();
        }
        BEAST_EXPECT(delivered < names.size());
        resolver.reset();
        pass();
    }

    void
    testStopAsync()
    {
        testcase("stop_async followed by stop is idempotent");
        using namespace std::chrono_literals;
        Loop loop;
        SuiteJournal journal("ResolverAsio_test", *this);
        auto resolver = ResolverAsio::New(loop.ioc, journal);
        resolver->start();

        Results results;
        resolver->resolve({"127.0.0.1:9"}, results.handler());
        BEAST_EXPECT(results.waitFor(1, 5000ms));
        resolver->stop_async();
        resolver->stop();
        resolver.reset();
        pass();
    }

public:
    void
    run() override
    {
        testNumericHost();
        testBatchSkipsUnparseable();
        testUnresolvableName();
        testStopWithPendingWork();
        testStopAsync();
    }
};

BEAST_DEFINE_TESTSUITE(ResolverAsio, basics, ripple);

}  // namespace test
}  // namespace ripple
