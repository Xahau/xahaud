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

#include <xrpl/beast/unit_test.h>
#include <xrpl/server/detail/Spawn.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace ripple {
namespace test {

// util::spawn wraps boost::asio::spawn (Boost 1.87+ removed the legacy
// overloads and the implicit strand): it runs the coroutine on a strand and
// propagates exceptions to io_context::run() instead of swallowing them.
class Spawn_test : public beast::unit_test::suite
{
    void
    testRunsOnGivenStrand()
    {
        testcase("a coroutine spawned on a strand runs and can suspend");
        boost::asio::io_context ioc;
        auto strand = boost::asio::make_strand(ioc);
        bool ran = false;
        bool suspended = false;
        util::spawn(strand, [&](boost::asio::yield_context yield) {
            suspended = strand.running_in_this_thread();
            boost::asio::steady_timer timer(ioc);
            timer.expires_after(std::chrono::milliseconds(1));
            timer.async_wait(yield);
            ran = strand.running_in_this_thread();
        });
        ioc.run();
        BEAST_EXPECT(suspended);
        BEAST_EXPECT(ran);
    }

    void
    testWrapsPlainExecutorInStrand()
    {
        testcase("a plain executor or context is wrapped in a strand");
        {
            boost::asio::io_context ioc;
            bool ran = false;
            util::spawn(ioc.get_executor(), [&](boost::asio::yield_context) {
                ran = true;
            });
            ioc.run();
            BEAST_EXPECT(ran);
        }
        {
            boost::asio::io_context ioc;
            bool ran = false;
            util::spawn(ioc, [&](boost::asio::yield_context) { ran = true; });
            ioc.run();
            BEAST_EXPECT(ran);
        }
    }

    void
    testPropagatesStdException()
    {
        testcase("a std::exception escapes io_context::run()");
        boost::asio::io_context ioc;
        util::spawn(
            boost::asio::make_strand(ioc), [](boost::asio::yield_context) {
                throw std::runtime_error("boom");
            });
        bool caught = false;
        try
        {
            ioc.run();
        }
        catch (std::runtime_error const& e)
        {
            caught = std::string(e.what()) == "boom";
        }
        BEAST_EXPECT(caught);
    }

    void
    testPropagatesUnknownException()
    {
        testcase("a non-std exception escapes io_context::run()");
        boost::asio::io_context ioc;
        util::spawn(
            boost::asio::make_strand(ioc),
            [](boost::asio::yield_context) { throw 42; });
        bool caught = false;
        try
        {
            ioc.run();
        }
        catch (int v)
        {
            caught = v == 42;
        }
        BEAST_EXPECT(caught);
    }

public:
    void
    run() override
    {
        testRunsOnGivenStrand();
        testWrapsPlainExecutorInStrand();
        testPropagatesStdException();
        testPropagatesUnknownException();
    }
};

BEAST_DEFINE_TESTSUITE(Spawn, server, ripple);

}  // namespace test
}  // namespace ripple
