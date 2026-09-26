#include <xrpl/beast/unit_test.h>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <string>

namespace ripple {
namespace test {

class BeastTimeout_test : public beast::unit_test::suite
{
    // Regression for boostorg/beast#2925, fixed by #2926. Boost 1.86 can
    // move the operation before reading its executor in the expired path.
    template <class Initiate>
    void
    testExpired(std::string const& name, Initiate const& initiate)
    {
        namespace net = boost::asio;
        for (bool onStrand : {false, true})
        {
            testcase(name + (onStrand ? " on strand" : " outside strand"));
            net::io_context ioc;
            // Match the server's type-erased TS executor and strand.
            boost::beast::basic_stream<net::ip::tcp, net::executor> stream(ioc);
            net::strand<net::executor> strand(ioc.get_executor());
            // Exercise the manual timeout branch without a timer/sleep race.
            stream.expires_after(-std::chrono::seconds(1));
            int completions = 0;
            bool initiating = true;
            auto handler = net::bind_executor(
                strand, [&](boost::system::error_code ec, std::size_t n) {
                    ++completions;
                    BEAST_EXPECT(!initiating);
                    BEAST_EXPECT(strand.running_in_this_thread());
                    BEAST_EXPECT(ec == boost::beast::error::timeout);
                    BEAST_EXPECT(n == 0);
                });
            auto start = [&] {
                // The expired path runs before socket I/O, so no connection
                // is needed. Stock Boost 1.86 throws bad_executor with GCC.
                initiate(stream, handler);
                initiating = false;
            };
            if (onStrand)
                net::post(strand, start);
            else
                start();
            ioc.run();
            BEAST_EXPECT(completions == 1);
        }
    }

    void
    run() override
    {
        namespace net = boost::asio;
        namespace http = boost::beast::http;

        char byte = 0;
        testExpired("stream read", [&](auto& stream, auto handler) {
            stream.async_read_some(net::buffer(&byte, 1), handler);
        });
        testExpired("stream write", [&](auto& stream, auto handler) {
            stream.async_write_some(net::buffer(&byte, 1), handler);
        });

        // Exercise completion through HTTP's composed operations too. The
        // reproducer in boostorg/beast#2941 writes HTTP after stream expiry.
        boost::beast::flat_buffer buffer;
        http::request<http::string_body> incoming;
        testExpired("HTTP read", [&](auto& stream, auto handler) {
            http::async_read(stream, buffer, incoming, handler);
        });
        http::request<http::string_body> outgoing{http::verb::post, "/", 11};
        outgoing.body() = "test";
        outgoing.prepare_payload();
        testExpired("HTTP write", [&](auto& stream, auto handler) {
            http::async_write(stream, outgoing, handler);
        });
    }
};

BEAST_DEFINE_TESTSUITE(BeastTimeout, server, ripple);

}  // namespace test
}  // namespace ripple
