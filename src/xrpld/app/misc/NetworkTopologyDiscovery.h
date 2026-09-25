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
    OR  IN  CONNECTION  WITH  THE  USE  OR  PERFORMANCE  OF  THIS  SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_APP_MISCTOPOLOGYDISCOVERY_H_INCLUDED
#define RIPPLE_APP_MISCTOPOLOGYDISCOVERY_H_INCLUDED

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/basics/chrono.h>
#include <xrpld/peerfinder/PeerfinderManager.h>
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/container/flat_set.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace xrpl::xahau {

/** Network topology discovery via "ping loops".

    Periodically sends XUSHPING packets into the UDP highway.  Each peer that
    receives the ping adds its node ID to the hop list and forwards it on.
    When the packet eventually returns to the originating node (via a different
    peer connection), we have a trace of the path through the network.

    The hop list is a bloom-like trace of which nodes the ping passed through.
    Repeating traces allow us to identify hubs (frequently-traversed nodes) and
    adjust our peering strategy to connect closer to those hubs.
*/
class NetworkTopologyDiscovery
{
public:
    static constexpr int kMaxHopCount = 32;
    static constexpr uint64_t kPingFeeXRP = 10000000;  // 10 XRP per ping
    static constexpr int kPingTTL = 16;                // max hops before expiry

    NetworkTopologyDiscovery(
        ripple::Application& app,
        beast::Journal j,
        boost::asio::io_service& ios)
        : app_(app), j_(j), ios_(ios), pingTimer_(ios)
    {
        // Start periodic ping timer
        pingTimer_.expires_after(std::chrono::minutes(5));
        pingTimer_.async_wait(std::bind(
            &NetworkTopologyDiscovery::onPingTimer,
            this,
            std::placeholders::_1));
    }

    ~NetworkTopologyDiscovery()
    {
        pingTimer_.cancel();
    }

    /** Handle an incoming XUSHPING packet.

        @param pingData  The payload after the XUSHPING header
        @param sender    TCP endpoint of the direct sender (for forwarding)
        @param myNodeId  Our node public key
    */
    void
    onPingReceived(
        std::vector<uint8_t> const& pingData,
        boost::asio::ip::tcp::endpoint const& sender,
        std::vector<uint8_t> const& myNodeId);

    /** Build a list of recommended peers to connect to, based on hub
        frequency data. */
    std::vector<beast::IP::Endpoint>
    getRecommendedPeers();

    /** Whether we are in "hidden mode" — we participate in consensus but
        don't advertise ourselves to the network. */
    bool
    isHiddenMode() const
    {
        return hiddenMode_;
    }

private:
    void
    onPingTimer(boost::system::error_code const& ec);
    void
    sendPing();

    /** Record a node ID seen in a ping trace.  High-frequency nodes are
        classified as hubs. */
    void
    recordHop(std::vector<uint8_t> const& nodeId);

    /** Check whether a node ID is already in the hop list. */
    bool
    hopListContains(
        std::vector<uint8_t> const& pingData,
        std::vector<uint8_t> const& nodeId);

    /** Add a node ID to the hop list. */
    void
    addToHopList(std::vector<uint8_t>& pingData);

    /** Build the XUSHPING packet.

        Format:
          [4 bytes: sender node ID length]
          [N bytes: sender node ID]
          [2 bytes: hop count]
          [hop_count * (4 + node_id_len) bytes: hop list entries]
          [4 bytes: TTL (big-endian uint32)]
    */
    std::vector<uint8_t>
    buildPingPacket(std::vector<uint8_t> const& myNodeId);

    /** Process a returned ping and extract topology information. */
    void
    processReturnedPing(
        std::vector<uint8_t> const& pingData,
        boost::asio::ip::tcp::endpoint const& sender);

    ripple::Application& app_;
    beast::Journal const j_;
    boost::asio::io_service& ios_;
    boost::asio::basic_waitable_timer<std::chrono::steady_clock> pingTimer_;  // Initialized in constructor

    // Hidden mode: don't advertise ourselves
    bool hiddenMode_ = false;

    // Hub tracking: node_id -> frequency count
    std::unordered_map<std::string, uint64_t> hubFrequency_;
    mutable std::mutex hubMutex_;
};

}  // namespace xrpl::xahau

#endif
