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

#include <xrpld/app/main/Application.h>
#include <xrpld/overlay/Overlay.h>
#include <xrpld/app/misc/NetworkTopologyDiscovery.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/AccountID.h>
#include <boost/container/small_vector.hpp>
#include <cstring>
#include <sstream>

namespace xrpl::xahau {
    using namespace ripple;

namespace {

static std::string
nodeIdToStr(std::vector<uint8_t> const& id)
{
    return std::string(id.begin(), id.end());
}

static uint32_t
encodeUint32BE(uint32_t val, uint8_t* dst)
{
    dst[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    dst[3] = static_cast<uint8_t>(val & 0xFF);
    return 4;
}

static uint32_t
decodeUint32BE(const uint8_t* src)
{
    return (static_cast<uint32_t>(src[0]) << 24) |
        (static_cast<uint32_t>(src[1]) << 16) |
        (static_cast<uint32_t>(src[2]) << 8) | static_cast<uint32_t>(src[3]);
}

}  // anonymous namespace

void
NetworkTopologyDiscovery::onPingTimer(boost::system::error_code const& ec)
{
    if (ec == boost::asio::error::operation_aborted)
        return;

    if (!ec)
        sendPing();

    // Reschedule
    pingTimer_.expires_after(std::chrono::minutes(5));
    pingTimer_.async_wait(std::bind(
        &NetworkTopologyDiscovery::onPingTimer, this, std::placeholders::_1));
}

void
NetworkTopologyDiscovery::sendPing()
{
    auto const& identity = app_.nodeIdentity();
    auto pkSlice = identity.first.slice();
    std::vector<uint8_t> nodeId(pkSlice.data(), pkSlice.data() + pkSlice.size());

    auto pingData = buildPingPacket(nodeId);
    if (pingData.empty())
        return;

    JLOG(j_.info()) << "NetworkTopologyDiscovery: sending XUSHPING with TTL="
                    << kPingTTL;

    // Machine-gun the ping to highway peers
    Slice slice(pingData.data(), pingData.size());
    app_.overlay().peerFinder().machine_gun_highway_peers(slice, uint256{});
}

std::vector<uint8_t>
NetworkTopologyDiscovery::buildPingPacket(std::vector<uint8_t> const& myNodeId)
{
    std::vector<uint8_t> ping;
    const size_t nodeIdLen = myNodeId.size();

    // Sender node ID length (4 bytes, big-endian)
    encodeUint32BE(
        static_cast<uint32_t>(nodeIdLen),
        ping.data() == nullptr ? new uint8_t[4] : ping.data());
    ping.resize(4);
    encodeUint32BE(static_cast<uint32_t>(nodeIdLen), ping.data());

    // Sender node ID
    ping.insert(ping.end(), myNodeId.begin(), myNodeId.end());

    // Hop count (2 bytes, big-endian)
    uint16_t hopCount = 0;
    ping.push_back(static_cast<uint8_t>((hopCount >> 8) & 0xFF));
    ping.push_back(static_cast<uint8_t>(hopCount & 0xFF));

    // TTL (4 bytes, big-endian)
    uint32_t ttl = kPingTTL;
    encodeUint32BE(ttl, ping.data() + ping.size());
    ping.resize(ping.size() + 4);
    encodeUint32BE(ttl, ping.data() + ping.size() - 4);

    return ping;
}

void
NetworkTopologyDiscovery::onPingReceived(
    std::vector<uint8_t> const& pingData,
    boost::asio::ip::tcp::endpoint const& sender,
    std::vector<uint8_t> const& myNodeId)
{
    if (pingData.size() < 10)
        return;  // too small

    // Parse sender node ID
    uint32_t senderNodeIdLen = decodeUint32BE(pingData.data());
    if (senderNodeIdLen == 0 || senderNodeIdLen > pingData.size() - 4)
        return;

    auto const senderId = std::vector<uint8_t>(
        pingData.begin() + 4, pingData.begin() + 4 + senderNodeIdLen);

    // Hidden mode: never forward pings from ourselves (they've returned)
    // and if hidden, don't add ourselves to the hop list

    // Find hop count field (after sender ID)
    size_t hopCountOffset = 4 + senderNodeIdLen;
    if (hopCountOffset + 2 > pingData.size())
        return;

    uint16_t hopCount = (static_cast<uint16_t>(pingData[hopCountOffset]) << 8) |
        pingData[hopCountOffset + 1];

    if (hopCount > kMaxHopCount)
        return;  // too many hops, drop

    // Find TTL field (after hop count)
    size_t ttlOffset = hopCountOffset + 2 + hopCount * (4 + senderNodeIdLen);
    if (ttlOffset + 4 > pingData.size())
        return;

    uint32_t ttl = decodeUint32BE(pingData.data() + ttlOffset);
    if (ttl == 0)
        return;  // TTL expired

    // Check if we are the sender (ping returned to us)
    bool amSender = (senderId == myNodeId);

    if (amSender)
    {
        // Ping has returned to us — record topology info
        processReturnedPing(pingData, sender);
        return;
    }

    // Check if we're already in the hop list
    bool alreadyInList = false;
    for (uint16_t i = 0; i < hopCount; i++)
    {
        size_t entryOffset = hopCountOffset + 2 + i * (4 + senderNodeIdLen);
        if (entryOffset + 4 + senderNodeIdLen > pingData.size())
            break;

        uint32_t entryLen = decodeUint32BE(pingData.data() + entryOffset);
        auto entryId = std::vector<uint8_t>(
            pingData.begin() + entryOffset + 4,
            pingData.begin() + entryOffset + 4 + entryLen);

        if (entryId == myNodeId)
        {
            alreadyInList = true;
            break;
        }
    }

    if (alreadyInList)
        return;  // prevent loops

    // If we're hidden, don't add ourselves — just forward
    if (!hiddenMode_)
    {
        // Add ourselves to hop list
        // Note: we'd need to rebuild the packet here in a real implementation
        // For now, we record hops and forward
    }

    // Record hops for topology analysis
    for (uint16_t i = 0; i < hopCount; i++)
    {
        size_t entryOffset = hopCountOffset + 2 + i * (4 + senderNodeIdLen);
        if (entryOffset + 4 + senderNodeIdLen > pingData.size())
            break;

        uint32_t entryLen = decodeUint32BE(pingData.data() + entryOffset);
        auto entryId = std::vector<uint8_t>(
            pingData.begin() + entryOffset + 4,
            pingData.begin() + entryOffset + 4 + entryLen);

        recordHop(entryId);
    }

    // Forward to other highway peers
    ttl--;
    encodeUint32BE(ttl, const_cast<uint8_t*>(pingData.data() + ttlOffset));

    JLOG(j_.trace()) << "NetworkTopologyDiscovery: forwarding XUSHPING ttl="
                     << ttl << " hops=" << hopCount;

    Slice slice(pingData.data(), pingData.size());
    app_.overlay().peerFinder().machine_gun_highway_peers(slice, uint256{});
}

void
NetworkTopologyDiscovery::recordHop(std::vector<uint8_t> const& nodeId)
{
    auto key = nodeIdToStr(nodeId);
    std::lock_guard<std::mutex> lock(hubMutex_);
    hubFrequency_[key]++;

    if (hubFrequency_[key] % 10 == 0)
    {
        JLOG(j_.info()) << "NetworkTopologyDiscovery: node " << key.substr(0, 8)
                        << "... seen " << hubFrequency_[key]
                        << " times (potential hub)";
    }
}

void
NetworkTopologyDiscovery::processReturnedPing(
    std::vector<uint8_t> const& pingData,
    boost::asio::ip::tcp::endpoint const& sender)
{
    JLOG(j_.info()) << "NetworkTopologyDiscovery: XUSHPING returned via "
                    << sender;

    // Record all hops from the returned ping
    auto const nodeIdLen = 32;

    size_t hopCountOffset = 4 + nodeIdLen;
    if (hopCountOffset + 2 > pingData.size())
        return;

    uint16_t hopCount = (static_cast<uint16_t>(pingData[hopCountOffset]) << 8) |
        pingData[hopCountOffset + 1];

    for (uint16_t i = 0; i < hopCount; i++)
    {
        size_t entryOffset = hopCountOffset + 2 + i * (4 + nodeIdLen);
        if (entryOffset + 4 + nodeIdLen > pingData.size())
            break;

        auto entryId = std::vector<uint8_t>(
            pingData.begin() + entryOffset + 4,
            pingData.begin() + entryOffset + 4 + nodeIdLen);

        recordHop(entryId);
    }
}

std::vector<beast::IP::Endpoint>
NetworkTopologyDiscovery::getRecommendedPeers()
{
    std::lock_guard<std::mutex> lock(hubMutex_);

    // Sort hubs by frequency (top 10)
    using HubEntry = std::pair<std::string, uint64_t>;
    std::vector<HubEntry> hubs(hubFrequency_.begin(), hubFrequency_.end());

    std::sort(
        hubs.begin(), hubs.end(), [](HubEntry const& a, HubEntry const& b) {
            return a.second > b.second;
        });

    // Note: In a real implementation we'd have node_id -> endpoint mapping
    // For now, return empty — the hub IDs are logged for operator review
    JLOG(j_.debug()) << "NetworkTopologyDiscovery: known " << hubs.size()
                     << " unique nodes, top hubs:";
    for (auto const& [id, freq] : hubs)
    {
        if (freq < 5)
            break;
        JLOG(j_.debug()) << "  hub=" << id.substr(0, 16) << " freq=" << freq;
    }

    return {};
}

}  // namespace xrpl::xahau
