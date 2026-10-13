//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <xrpld/core/ConfigSections.h>
#include <xrpl/basics/make_SSLContext.h>
#include <xrpl/beast/rfc2616.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/server/Server.h>
#include <xrpl/server/Session.h>

#include <test/jtx.h>
#include <test/jtx/CaptureLogs.h>
#include <test/jtx/envconfig.h>
#include <test/unit_test/SuiteJournal.h>

#include <boost/asio.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/utility/in_place_factory.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace ripple {
namespace test {

using socket_type = boost::beast::tcp_stream;
using stream_type = boost::beast::ssl_stream<socket_type>;

class Server_test : public beast::unit_test::suite
{
public:
    class TestThread
    {
    private:
        boost::asio::io_context io_context_;
        std::optional<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>>
            work_;
        std::thread thread_;

    public:
        TestThread()
            : work_(std::in_place, boost::asio::make_work_guard(io_context_))
            , thread_([&]() { this->io_context_.run(); })
        {
        }

        ~TestThread()
        {
            work_.reset();
            thread_.join();
        }

        boost::asio::io_context&
        get_io_context()
        {
            return io_context_;
        }
    };

    //--------------------------------------------------------------------------

    class TestSink : public beast::Journal::Sink
    {
        beast::unit_test::suite& suite_;

    public:
        explicit TestSink(beast::unit_test::suite& suite)
            : Sink(beast::severities::kWarning, false), suite_(suite)
        {
        }

        void
        write(beast::severities::Severity level, std::string const& text)
            override
        {
            if (level < threshold())
                return;

            suite_.log << text << std::endl;
        }

        void
        writeAlways(beast::severities::Severity level, std::string const& text)
            override
        {
            suite_.log << text << std::endl;
        }
    };

    //--------------------------------------------------------------------------

    struct TestHandler
    {
        bool
        onAccept(Session& session, boost::asio::ip::tcp::endpoint endpoint)
        {
            return true;
        }

        Handoff
        onHandoff(
            Session& session,
            std::unique_ptr<stream_type>&& bundle,
            http_request_type&& request,
            boost::asio::ip::tcp::endpoint remote_address)
        {
            return Handoff{};
        }

        Handoff
        onHandoff(
            Session& session,
            http_request_type&& request,
            boost::asio::ip::tcp::endpoint remote_address)
        {
            return Handoff{};
        }

        void
        onRequest(Session& session)
        {
            session.write(std::string("Hello, world!\n"));
            if (beast::rfc2616::is_keep_alive(session.request()))
                session.complete();
            else
                session.close(true);
        }

        void
        onWSMessage(
            std::shared_ptr<WSSession> session,
            std::vector<boost::asio::const_buffer> const&)
        {
        }

        void
        onUDPMessage(
            std::string const& message,
            boost::asio::ip::tcp::endpoint const& remoteEndpoint,
            Port const& port,
            std::function<void(std::string const&)> sendResponse)
        {
        }

        void
        onClose(Session& session, boost::system::error_code const&)
        {
        }

        void
        onStopped(Server& server)
        {
        }
    };

    //--------------------------------------------------------------------------

    // Connect to an address
    template <class Socket>
    bool
    connect(Socket& s, typename Socket::endpoint_type const& ep)
    {
        try
        {
            s.connect(ep);
            pass();
            return true;
        }
        catch (std::exception const& e)
        {
            fail(e.what());
        }

        return false;
    }

    // Write a string to the stream
    template <class SyncWriteStream>
    bool
    write(SyncWriteStream& s, std::string const& text)
    {
        try
        {
            boost::asio::write(s, boost::asio::buffer(text));
            pass();
            return true;
        }
        catch (std::exception const& e)
        {
            fail(e.what());
        }
        return false;
    }

    // Expect that reading the stream produces a matching string
    template <class SyncReadStream>
    bool
    expect_read(SyncReadStream& s, std::string const& match)
    {
        boost::asio::streambuf b(1000);  // limit on read
        try
        {
            auto const n = boost::asio::read_until(s, b, '\n');
            if (BEAST_EXPECT(n == match.size()))
            {
                std::string got;
                got.resize(n);
                boost::asio::buffer_copy(
                    boost::asio::buffer(&got[0], n), b.data());
                return BEAST_EXPECT(got == match);
            }
        }
        catch (std::length_error const& e)
        {
            fail(e.what());
        }
        catch (std::exception const& e)
        {
            fail(e.what());
        }
        return false;
    }

    void
    test_request(boost::asio::ip::tcp::endpoint const& ep)
    {
        boost::asio::io_context ios;
        using socket = boost::asio::ip::tcp::socket;
        socket s(ios);

        if (!connect(s, ep))
            return;

        if (!write(
                s,
                "GET / HTTP/1.1\r\n"
                "Connection: close\r\n"
                "\r\n"))
            return;

        if (!expect_read(s, "Hello, world!\n"))
            return;

        boost::system::error_code ec;
        s.shutdown(socket::shutdown_both, ec);

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    void
    test_keepalive(boost::asio::ip::tcp::endpoint const& ep)
    {
        boost::asio::io_context ios;
        using socket = boost::asio::ip::tcp::socket;
        socket s(ios);

        if (!connect(s, ep))
            return;

        if (!write(
                s,
                "GET / HTTP/1.1\r\n"
                "Connection: Keep-Alive\r\n"
                "\r\n"))
            return;

        if (!expect_read(s, "Hello, world!\n"))
            return;

        if (!write(
                s,
                "GET / HTTP/1.1\r\n"
                "Connection: close\r\n"
                "\r\n"))
            return;

        if (!expect_read(s, "Hello, world!\n"))
            return;

        boost::system::error_code ec;
        s.shutdown(socket::shutdown_both, ec);
    }

    void
    basicTests()
    {
        testcase("Basic client/server");
        TestSink sink{*this};
        TestThread thread;
        sink.threshold(beast::severities::Severity::kAll);
        beast::Journal journal{sink};
        TestHandler handler;
        auto s = make_Server(handler, thread.get_io_context(), journal);
        std::vector<Port> serverPort(1);
        serverPort.back().ip =
            boost::asio::ip::make_address(getEnvLocalhostAddr()),
        serverPort.back().port = 0;
        serverPort.back().protocol.insert("http");
        auto eps = s->ports(serverPort);
        test_request(eps.begin()->second);
        test_keepalive(eps.begin()->second);
        // s->close();
        s = nullptr;
        pass();
    }

    void
    stressTest()
    {
        testcase("stress test");
        struct NullHandler
        {
            bool
            onAccept(Session& session, boost::asio::ip::tcp::endpoint endpoint)
            {
                return true;
            }

            Handoff
            onHandoff(
                Session& session,
                std::unique_ptr<stream_type>&& bundle,
                http_request_type&& request,
                boost::asio::ip::tcp::endpoint remote_address)
            {
                return Handoff{};
            }

            Handoff
            onHandoff(
                Session& session,
                http_request_type&& request,
                boost::asio::ip::tcp::endpoint remote_address)
            {
                return Handoff{};
            }

            void
            onRequest(Session& session)
            {
            }

            void
            onWSMessage(
                std::shared_ptr<WSSession> session,
                std::vector<boost::asio::const_buffer> const& buffers)
            {
            }

            void
            onUDPMessage(
                std::string const& message,
                boost::asio::ip::tcp::endpoint const& remoteEndpoint,
                Port const& port,
                std::function<void(std::string const&)> sendResponse)
            {
            }

            void
            onClose(Session& session, boost::system::error_code const&)
            {
            }

            void
            onStopped(Server& server)
            {
            }
        };

        using namespace beast::severities;
        SuiteJournal journal("Server_test", *this);

        NullHandler h;
        for (int i = 0; i < 1000; ++i)
        {
            TestThread thread;
            auto s = make_Server(h, thread.get_io_context(), journal);
            std::vector<Port> serverPort(1);
            serverPort.back().ip =
                boost::asio::ip::make_address(getEnvLocalhostAddr()),
            serverPort.back().port = 0;
            serverPort.back().protocol.insert("http");
            s->ports(serverPort);
        }
        pass();
    }

    // Records the datagrams delivered to onUDPMessage
    struct UDPHandler : TestHandler
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::pair<std::string, bool>> received;

        void
        onUDPMessage(
            std::string const& message,
            boost::asio::ip::tcp::endpoint const& remoteEndpoint,
            Port const& port,
            std::function<void(std::string const&)> sendResponse)
        {
            std::lock_guard lock(mutex);
            received.emplace_back(message, port.has_peer());
            cv.notify_all();
        }

        bool
        wait(std::size_t count, std::chrono::milliseconds timeout)
        {
            std::unique_lock lock(mutex);
            return cv.wait_for(
                lock, timeout, [&] { return received.size() >= count; });
        }
    };

    // A peer port with udp_highway set also accepts UDP datagrams on the
    // same port number; one without it does not.
    void
    udpHighwayTests()
    {
        testcase("UDP Superhighway door");
        using namespace std::chrono_literals;
        using udp = boost::asio::ip::udp;

        TestSink sink{*this};
        TestThread thread;
        sink.threshold(beast::severities::Severity::kAll);
        beast::Journal journal{sink};

        auto const addr = boost::asio::ip::make_address(getEnvLocalhostAddr());

        auto run = [&](bool highway) {
            UDPHandler handler;
            auto s = make_Server(handler, thread.get_io_context(), journal);

            std::vector<Port> ports(1);
            ports.back().name = "peer";
            ports.back().ip = addr;
            ports.back().port = 0;
            ports.back().protocol.insert("peer");
            ports.back().udp_highway = highway;

            auto const eps = s->ports(ports);
            if (!BEAST_EXPECT(eps.size() == 1))
                return;

            // The TCP door picked the port number; the UDP door shares it.
            auto const port = eps.begin()->second.port();
            BEAST_EXPECT(port != 0);

            boost::asio::io_context io;
            udp::socket client(io, udp::endpoint(addr, 0));
            std::string const message = "XUSHTEST";
            client.send_to(
                boost::asio::buffer(message), udp::endpoint(addr, port));

            if (highway)
            {
                if (BEAST_EXPECT(handler.wait(1, 5s)))
                {
                    std::lock_guard lock(handler.mutex);
                    BEAST_EXPECT(handler.received.front().first == message);
                    BEAST_EXPECT(handler.received.front().second);
                }
            }
            else
            {
                BEAST_EXPECT(!handler.wait(1, 200ms));
            }

            s = nullptr;
        };

        run(true);
        run(false);
    }

    // Answers every datagram through sendResponse: "big" gets a reply too
    // large for one datagram, anything else gets "pong".
    struct UDPEchoHandler : TestHandler
    {
        // Two fragments: small enough not to overflow a default socket buffer.
        static constexpr std::size_t bigSize = 100000;

        void
        onUDPMessage(
            std::string const& message,
            boost::asio::ip::tcp::endpoint const& remoteEndpoint,
            Port const& port,
            std::function<void(std::string const&)> sendResponse)
        {
            if (message == "big")
            {
                std::string big(bigSize, '\0');
                for (std::size_t i = 0; i < big.size(); ++i)
                    big[i] = static_cast<char>('a' + (i % 26));
                sendResponse(big);
            }
            else
            {
                sendResponse("pong");
            }
        }

        static std::string
        expectedBig()
        {
            std::string big(bigSize, '\0');
            for (std::size_t i = 0; i < big.size(); ++i)
                big[i] = static_cast<char>('a' + (i % 26));
            return big;
        }
    };

    // Exercises UDPDoor::do_send / send_datagram: a short reply goes out as
    // one datagram, a long one is split into headered fragments.
    void
    udpResponseTests()
    {
        testcase("UDP RPC door responses");
        using namespace std::chrono_literals;
        using udp = boost::asio::ip::udp;

        TestSink sink{*this};
        TestThread thread;
        sink.threshold(beast::severities::Severity::kAll);
        beast::Journal journal{sink};

        auto const addr = boost::asio::ip::make_address(getEnvLocalhostAddr());

        // UDPDoor reports the configured port, not the bound one, so pick a
        // free port up front.
        std::uint16_t port = 0;
        {
            boost::asio::io_context io;
            udp::socket probe(io, udp::endpoint(addr, 0));
            port = probe.local_endpoint().port();
        }

        UDPEchoHandler handler;
        auto s = make_Server(handler, thread.get_io_context(), journal);

        std::vector<Port> ports(1);
        ports.back().name = "udp_rpc";
        ports.back().ip = addr;
        ports.back().port = port;
        ports.back().protocol.insert("udp");
        s->ports(ports);

        boost::asio::io_context io;
        udp::socket client(io, udp::endpoint(addr, 0));
        // Room for the full fragmented reply on platforms with small
        // default socket buffers.
        client.set_option(udp::socket::receive_buffer_size(512 * 1024));
        udp::endpoint const server(addr, port);

        // Receive one datagram, or nothing if the timeout expires.
        auto receive = [&](std::chrono::milliseconds timeout)
            -> std::optional<std::string> {
            std::vector<char> buf(70000);
            udp::endpoint from;
            std::optional<std::string> result;
            client.async_receive_from(
                boost::asio::buffer(buf),
                from,
                [&](boost::system::error_code ec, std::size_t n) {
                    if (!ec)
                        result.emplace(buf.data(), n);
                });
            io.restart();
            io.run_for(timeout);
            if (!result)
            {
                client.cancel();
                io.restart();
                io.run();
            }
            return result;
        };

        // Single datagram reply
        client.send_to(boost::asio::buffer(std::string("ping")), server);
        if (auto const r = receive(5s); BEAST_EXPECT(r))
            BEAST_EXPECT(*r == "pong");

        // Fragmented reply
        client.send_to(boost::asio::buffer(std::string("big")), server);
        std::size_t constexpr headerSize = 16;
        std::size_t constexpr payloadSize = 65487 - headerSize;
        std::size_t const expectedPackets =
            (UDPEchoHandler::bigSize + payloadSize - 1) / payloadSize;

        std::map<std::uint16_t, std::string> fragments;
        std::uint16_t total = 0;
        while (fragments.size() < expectedPackets)
        {
            auto const r = receive(5s);
            if (!BEAST_EXPECT(r) || !BEAST_EXPECT(r->size() > headerSize))
                break;
            auto const byte = [&](std::size_t i) {
                return static_cast<std::uint16_t>(
                    static_cast<unsigned char>((*r)[i]));
            };
            BEAST_EXPECT(byte(0) == 0 && byte(1) == 0);
            BEAST_EXPECT(byte(2) == 0 && byte(3) == 0);
            std::uint16_t const num = byte(4) | (byte(5) << 8);
            total = byte(6) | (byte(7) << 8);
            fragments[num] = r->substr(headerSize);
        }

        BEAST_EXPECT(total == expectedPackets);
        if (BEAST_EXPECT(fragments.size() == expectedPackets))
        {
            std::string reassembled;
            for (auto const& [num, data] : fragments)
                reassembled += data;
            BEAST_EXPECT(reassembled == UDPEchoHandler::expectedBig());
        }

        s = nullptr;
    }

    void
    testBadConfig()
    {
        testcase("Server config - invalid options");
        using namespace test::jtx;

        std::string messages;

        except([&] {
            Env env{
                *this,
                envconfig([](std::unique_ptr<Config> cfg) {
                    (*cfg).deprecatedClearSection("port_rpc");
                    return cfg;
                }),
                std::make_unique<CaptureLogs>(&messages)};
        });
        BEAST_EXPECT(
            messages.find("Missing 'ip' in [port_rpc]") != std::string::npos);

        except([&] {
            Env env{
                *this,
                envconfig([](std::unique_ptr<Config> cfg) {
                    (*cfg).deprecatedClearSection("port_rpc");
                    (*cfg)["port_rpc"].set("ip", getEnvLocalhostAddr());
                    return cfg;
                }),
                std::make_unique<CaptureLogs>(&messages)};
        });
        BEAST_EXPECT(
            messages.find("Missing 'port' in [port_rpc]") != std::string::npos);

        except([&] {
            Env env{
                *this,
                envconfig([](std::unique_ptr<Config> cfg) {
                    (*cfg).deprecatedClearSection("port_rpc");
                    (*cfg)["port_rpc"].set("ip", getEnvLocalhostAddr());
                    (*cfg)["port_rpc"].set("port", "0");
                    return cfg;
                }),
                std::make_unique<CaptureLogs>(&messages)};
        });
        BEAST_EXPECT(
            messages.find("Invalid value '0' for key 'port' in [port_rpc]") ==
            std::string::npos);

        except([&] {
            Env env{
                *this,
                envconfig([](std::unique_ptr<Config> cfg) {
                    (*cfg)["server"].set("port", "0");
                    return cfg;
                }),
                std::make_unique<CaptureLogs>(&messages)};
        });
        BEAST_EXPECT(
            messages.find("Invalid value '0' for key 'port' in [server]") !=
            std::string::npos);

        except([&] {
            Env env{
                *this,
                envconfig([](std::unique_ptr<Config> cfg) {
                    (*cfg).deprecatedClearSection("port_rpc");
                    (*cfg)["port_rpc"].set("ip", getEnvLocalhostAddr());
                    (*cfg)["port_rpc"].set("port", "8081");
                    (*cfg)["port_rpc"].set("protocol", "");
                    return cfg;
                }),
                std::make_unique<CaptureLogs>(&messages)};
        });
        BEAST_EXPECT(
            messages.find("Missing 'protocol' in [port_rpc]") !=
            std::string::npos);

        except(
            [&]  // this creates a standard test config without the server
                 // section
            {
                Env env{
                    *this,
                    envconfig([](std::unique_ptr<Config> cfg) {
                        cfg = std::make_unique<Config>();
                        cfg->overwrite(
                            ConfigSection::nodeDatabase(), "type", "memory");
                        cfg->overwrite(
                            ConfigSection::nodeDatabase(), "path", "main");
                        cfg->deprecatedClearSection(
                            ConfigSection::importNodeDatabase());
                        cfg->legacy("database_path", "");
                        cfg->setupControl(true, true, true);
                        (*cfg)["port_peer"].set("ip", getEnvLocalhostAddr());
                        (*cfg)["port_peer"].set("port", "8080");
                        (*cfg)["port_peer"].set("protocol", "peer");
                        (*cfg)["port_rpc"].set("ip", getEnvLocalhostAddr());
                        (*cfg)["port_rpc"].set("port", "8081");
                        (*cfg)["port_rpc"].set("protocol", "http,ws2");
                        (*cfg)["port_rpc"].set("admin", getEnvLocalhostAddr());
                        (*cfg)["port_ws"].set("ip", getEnvLocalhostAddr());
                        (*cfg)["port_ws"].set("port", "8082");
                        (*cfg)["port_ws"].set("protocol", "ws");
                        (*cfg)["port_ws"].set("admin", getEnvLocalhostAddr());
                        return cfg;
                    }),
                    std::make_unique<CaptureLogs>(&messages)};
            });
        BEAST_EXPECT(
            messages.find("Required section [server] is missing") !=
            std::string::npos);

        except([&]  // this creates a standard test config without some of the
                    // port sections
               {
                   Env env{
                       *this,
                       envconfig([](std::unique_ptr<Config> cfg) {
                           cfg = std::make_unique<Config>();
                           cfg->overwrite(
                               ConfigSection::nodeDatabase(), "type", "memory");
                           cfg->overwrite(
                               ConfigSection::nodeDatabase(), "path", "main");
                           cfg->deprecatedClearSection(
                               ConfigSection::importNodeDatabase());
                           cfg->legacy("database_path", "");
                           cfg->setupControl(true, true, true);
                           (*cfg)["server"].append("port_peer");
                           (*cfg)["server"].append("port_rpc");
                           (*cfg)["server"].append("port_ws");
                           return cfg;
                       }),
                       std::make_unique<CaptureLogs>(&messages)};
               });
        BEAST_EXPECT(
            messages.find("Missing section: [port_peer]") != std::string::npos);
    }

    void
    run() override
    {
        basicTests();
        stressTest();
        udpHighwayTests();
        udpResponseTests();
        testBadConfig();
    }
};

BEAST_DEFINE_TESTSUITE(Server, http, ripple);

}  // namespace test
}  // namespace ripple
