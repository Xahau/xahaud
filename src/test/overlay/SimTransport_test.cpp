#include <test/jtx/SimTransport.h>

#include <xrpl/beast/unit_test/suite.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ripple::test {

class SimTransport_test : public beast::unit_test::suite
{
    using Bytes = std::vector<std::uint8_t>;

    void
    testDelayedWriteCannotCrossClose()
    {
        testcase("routed read completion followed by close rejects delayed write");

        boost::asio::io_context ioc;
        auto const exec = Transport::executor_type(ioc.get_executor());
        SimPipe pipe;
        std::vector<std::function<void()>> routed;
        std::vector<Bytes> delayed;
        pipe.setDeliveryRouter([&](Transport::executor_type,
                                   std::function<void()> completion,
                                   std::string) { routed.push_back(std::move(completion)); });

        std::array<std::uint8_t, 1> readBuffer{};
        Transport::error_code firstEc;
        std::size_t firstN = 0;
        pipe.read(
            {boost::asio::buffer(readBuffer)}, exec, [&](Transport::error_code ec, std::size_t n) {
                firstEc = ec;
                firstN = n;
            });

        BEAST_EXPECT(pipe.writeRaw(Bytes{'A'}));
        BEAST_EXPECT(routed.size() == 1);

        pipe.setWriteFault(
            [](std::uint16_t, std::size_t) { return SimFault{false, 0, std::chrono::seconds{1}}; });
        pipe.setDelayedWriter([&](std::chrono::steady_clock::duration, Bytes bytes) {
            delayed.push_back(std::move(bytes));
        });
        BEAST_EXPECT(pipe.write(Bytes{'B'}));
        BEAST_EXPECT(delayed.size() == 1);

        pipe.close();
        BEAST_EXPECT(!pipe.writeRaw(std::move(delayed.front())));
        BEAST_EXPECT(pipe.bufferedBytes() == 0);

        routed.front()();
        BEAST_EXPECT(!firstEc && firstN == 1 && readBuffer[0] == 'A');

        Transport::error_code secondEc;
        std::size_t secondN = 1;
        pipe.read(
            {boost::asio::buffer(readBuffer)}, exec, [&](Transport::error_code ec, std::size_t n) {
                secondEc = ec;
                secondN = n;
            });
        BEAST_EXPECT(routed.size() == 2);
        routed.back()();
        BEAST_EXPECT(secondEc == boost::asio::error::eof && secondN == 0);
    }

    void
    testEndpointWriteAfterSever()
    {
        testcase("endpoint write after sever reports broken pipe");

        boost::asio::io_context ioc;
        auto const exec = Transport::executor_type(ioc.get_executor());
        SimWire wire;
        auto endpoints = wire.endpoints(exec, exec, uint256{});
        wire.sever();

        std::string const payload = "closed";
        bool completed = false;
        Transport::error_code writeEc;
        std::size_t writeN = payload.size();
        endpoints.first->async_write(
            {boost::asio::buffer(payload)}, exec, [&](Transport::error_code ec, std::size_t n) {
                completed = true;
                writeEc = ec;
                writeN = n;
            });

        BEAST_EXPECT(!completed);
        ioc.run();
        BEAST_EXPECT(completed);
        BEAST_EXPECT(writeEc == boost::asio::error::broken_pipe);
        BEAST_EXPECT(writeN == 0);
        BEAST_EXPECT(wire.bufferedBytes() == 0);
    }

    void
    testFaultDropIsSuccessfulWrite()
    {
        testcase("intentional fault drop completes write successfully");

        boost::asio::io_context ioc;
        auto const exec = Transport::executor_type(ioc.get_executor());
        SimWire wire;
        auto endpoints = wire.endpoints(exec, exec, uint256{});
        wire.setFault(true, [](std::uint16_t, std::size_t) { return SimFault{true}; });

        std::string const payload = "dropped";
        bool completed = false;
        Transport::error_code writeEc;
        std::size_t writeN = 0;
        endpoints.first->async_write(
            {boost::asio::buffer(payload)}, exec, [&](Transport::error_code ec, std::size_t n) {
                completed = true;
                writeEc = ec;
                writeN = n;
            });

        ioc.run();
        BEAST_EXPECT(completed);
        BEAST_EXPECT(!writeEc && writeN == payload.size());
        BEAST_EXPECT(wire.bufferedBytes() == 0);
    }

    void
    testBufferedBytesDrainBeforeEof()
    {
        testcase("bytes buffered before close drain before EOF");

        boost::asio::io_context ioc;
        auto const exec = Transport::executor_type(ioc.get_executor());
        SimPipe pipe;
        Bytes const payload{'A', 'B'};
        BEAST_EXPECT(pipe.writeRaw(payload));
        pipe.close();

        std::array<std::uint8_t, 4> readBuffer{};
        Transport::error_code firstEc;
        std::size_t firstN = 0;
        pipe.read(
            {boost::asio::buffer(readBuffer)}, exec, [&](Transport::error_code ec, std::size_t n) {
                firstEc = ec;
                firstN = n;
            });
        ioc.run();
        BEAST_EXPECT(!firstEc && firstN == payload.size());
        BEAST_EXPECT(std::equal(payload.begin(), payload.end(), readBuffer.begin()));

        ioc.restart();
        Transport::error_code secondEc;
        std::size_t secondN = 1;
        pipe.read(
            {boost::asio::buffer(readBuffer)}, exec, [&](Transport::error_code ec, std::size_t n) {
                secondEc = ec;
                secondN = n;
            });
        ioc.run();
        BEAST_EXPECT(secondEc == boost::asio::error::eof && secondN == 0);
    }

public:
    void
    run() override
    {
        testDelayedWriteCannotCrossClose();
        testEndpointWriteAfterSever();
        testFaultDropIsSuccessfulWrite();
        testBufferedBytesDrainBeforeEof();
    }
};

BEAST_DEFINE_TESTSUITE(SimTransport, overlay, ripple);

}  // namespace ripple::test
