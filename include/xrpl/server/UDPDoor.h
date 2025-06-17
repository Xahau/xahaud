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

#ifndef RIPPLE_SERVER_UDPDOOR_H_INCLUDED
#define RIPPLE_SERVER_UDPDOOR_H_INCLUDED

#include <ripple/basics/Log.h>
#include <ripple/basics/contract.h>
#include <ripple/server/impl/PlainHTTPPeer.h>
#include <ripple/server/impl/SSLHTTPPeer.h>
#include <ripple/server/impl/io_list.h>
#include <boost/asio/basic_waitable_timer.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/core/detect_ssl.hpp>
#include <boost/beast/core/multi_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/container/flat_map.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace ripple {

template <class Handler>
class UDPDoor : public io_list::work,
                public std::enable_shared_from_this<UDPDoor<Handler>>
{
private:
    using error_code = boost::system::error_code;
    using endpoint_type = boost::asio::ip::tcp::endpoint;
    using udp_socket = boost::asio::ip::udp::socket;

    beast::Journal const j_;
    Port const& port_;
    Handler& handler_;
    boost::asio::io_context& ioc_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    udp_socket socket_;
    std::vector<char> recv_buffer_;
    endpoint_type local_endpoint_;  // Store TCP-style endpoint

public:
    UDPDoor(
        Handler& handler,
        boost::asio::io_context& io_context,
        Port const& port,
        beast::Journal j)
        : j_(j)
        , port_(port)
        , handler_(handler)
        , ioc_(io_context)
        , strand_(io_context.get_executor())
        , socket_(io_context)
        , recv_buffer_(port.udp_packet_size)
        , local_endpoint_(port.ip, port.port)  // Store as TCP endpoint
    {
        error_code ec;

        // Create UDP endpoint from port configuration
        auto const addr = port_.ip.to_v4();
        boost::asio::ip::udp::endpoint udp_endpoint(addr, port_.port);

        socket_.open(boost::asio::ip::udp::v4(), ec);
        if (ec)
        {
            JLOG(j_.error()) << "UDP socket open failed: " << ec.message();
            return;
        }

        // Set socket options
        socket_.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec)
        {
            JLOG(j_.error())
                << "UDP set reuse_address failed: " << ec.message();
            return;
        }

        socket_.bind(udp_endpoint, ec);
        if (ec)
        {
            JLOG(j_.error()) << "UDP socket bind failed: " << ec.message();
            return;
        }

        JLOG(j_.info()) << "UDP-RPC listening on " << udp_endpoint;
    }

    endpoint_type
    get_endpoint() const
    {
        return local_endpoint_;
    }

    void
    run()
    {
        if (!socket_.is_open())
            return;

        do_receive();
    }

    void
    close() override
    {
        error_code ec;
        socket_.close(ec);
    }

private:
    void
    do_receive()
    {
        if (!socket_.is_open())
            return;

        socket_.async_receive_from(
            boost::asio::buffer(recv_buffer_),
            sender_endpoint_,
            boost::asio::bind_executor(
                strand_,
                std::bind(
                    &UDPDoor::on_receive,
                    this->shared_from_this(),
                    std::placeholders::_1,
                    std::placeholders::_2)));
    }

    void
    on_receive(error_code ec, std::size_t bytes_transferred)
    {
        if (ec)
        {
            if (ec != boost::asio::error::operation_aborted)
            {
                JLOG(j_.error()) << "UDP receive failed: " << ec.message();
                do_receive();
            }
            return;
        }

        // Convert UDP endpoint to TCP endpoint for compatibility
        endpoint_type tcp_endpoint(
            sender_endpoint_.address(), sender_endpoint_.port());

        // Handle the received UDP message
        handler_.onUDPMessage(
            std::string(recv_buffer_.data(), bytes_transferred),
            tcp_endpoint,
            [this, tcp_endpoint](std::string const& response) {
                do_send(response, tcp_endpoint);
            });

        do_receive();
    }

    void
    do_send(std::string const& response, endpoint_type const& tcp_endpoint)
    {
        if (!socket_.is_open())
        {
            std::cout << "UDP SOCKET NOT OPEN WHEN SENDING\n\n";
            return;
        }

        const size_t HEADER_SIZE = 16;
        const size_t MAX_DATAGRAM_SIZE =
            65487;  // Allow for ipv6 header 40 bytes + 8 bytes of udp header
        const size_t MAX_PAYLOAD_SIZE = MAX_DATAGRAM_SIZE - HEADER_SIZE;

        // Convert TCP endpoint back to UDP for sending
        boost::asio::ip::udp::endpoint udp_endpoint(
            tcp_endpoint.address(), tcp_endpoint.port());

        // If message fits in single datagram, send normally
        if (response.length() <= MAX_DATAGRAM_SIZE)
        {
            socket_.async_send_to(
                boost::asio::buffer(response),
                udp_endpoint,
                boost::asio::bind_executor(
                    strand_,
                    [this, self = this->shared_from_this()](
                        error_code ec, std::size_t bytes_transferred) {
                        if (ec && ec != boost::asio::error::operation_aborted)
                        {
                            JLOG(j_.error())
                                << "UDP send failed: " << ec.message();
                        }
                    }));
            return;
        }

        // Calculate number of packets needed
        const size_t payload_size = MAX_PAYLOAD_SIZE;
        const uint16_t total_packets =
            (response.length() + payload_size - 1) / payload_size;

        // Get current timestamp in microseconds
        auto now = std::chrono::system_clock::now();
        auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                          now.time_since_epoch())
                          .count();
        uint64_t timestamp = static_cast<uint64_t>(micros);

        // Send fragmented packets
        for (uint16_t packet_num = 0; packet_num < total_packets; packet_num++)
        {
            std::string fragment;
            fragment.reserve(MAX_DATAGRAM_SIZE);

            // Add header - 4 bytes of zeros
            fragment.push_back(0);
            fragment.push_back(0);
            fragment.push_back(0);
            fragment.push_back(0);

            // Add packet number (little endian)
            fragment.push_back(packet_num & 0xFF);
            fragment.push_back((packet_num >> 8) & 0xFF);

            // Add total packets (little endian)
            fragment.push_back(total_packets & 0xFF);
            fragment.push_back((total_packets >> 8) & 0xFF);

            // Add timestamp (8 bytes, little endian)
            fragment.push_back(timestamp & 0xFF);
            fragment.push_back((timestamp >> 8) & 0xFF);
            fragment.push_back((timestamp >> 16) & 0xFF);
            fragment.push_back((timestamp >> 24) & 0xFF);
            fragment.push_back((timestamp >> 32) & 0xFF);
            fragment.push_back((timestamp >> 40) & 0xFF);
            fragment.push_back((timestamp >> 48) & 0xFF);
            fragment.push_back((timestamp >> 56) & 0xFF);

            // Calculate payload slice
            size_t start = packet_num * payload_size;
            size_t length = std::min(payload_size, response.length() - start);
            fragment.append(response.substr(start, length));

            socket_.async_send_to(
                boost::asio::buffer(fragment),
                udp_endpoint,
                boost::asio::bind_executor(
                    strand_,
                    [this, self = this->shared_from_this()](
                        error_code ec, std::size_t bytes_transferred) {
                        if (ec && ec != boost::asio::error::operation_aborted)
                        {
                            JLOG(j_.error())
                                << "UDP send failed: " << ec.message();
                        }
                    }));
        }
    }

    boost::asio::ip::udp::endpoint sender_endpoint_;
};

}  // namespace ripple

#endif
