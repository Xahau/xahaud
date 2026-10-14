//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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

#include <xrpld/overlay/detail/XUSH.h>

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace ripple {
namespace xush {

namespace {

char const peersTag[tagSize] = {'X', 'U', 'S', 'H', 'P', 'E', 'E', 'R'};
char const txnTag[tagSize] = {'X', 'U', 'S', 'H', 'T', 'X', 'N', 'F'};

void
putU32(std::uint8_t* p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

std::uint32_t
getU32(std::uint8_t const* p)
{
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
        (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

}  // namespace

MessageType
classify(Slice datagram)
{
    if (datagram.size() < tagSize)
        return MessageType::unknown;

    if (std::memcmp(datagram.data(), peersTag, tagSize) == 0)
        return MessageType::peers;

    if (std::memcmp(datagram.data(), txnTag, tagSize) == 0)
        return MessageType::txn;

    return MessageType::unknown;
}

Buffer
encodePeers(std::vector<beast::IP::Endpoint> const& endpoints)
{
    std::vector<beast::IP::Endpoint const*> v4;
    std::vector<beast::IP::Endpoint const*> v6;

    for (auto const& ep : endpoints)
    {
        if (v4.size() + v6.size() == maxAdvertisedPeers)
            break;

        if (ep.address().is_v4())
            v4.push_back(&ep);
        else
            v6.push_back(&ep);
    }

    Buffer datagram(tagSize + 2 + v4.size() * 8 + v6.size() * 20);
    auto p = datagram.data();

    std::memcpy(p, peersTag, tagSize);
    p += tagSize;
    *p++ = static_cast<std::uint8_t>(v4.size());
    *p++ = static_cast<std::uint8_t>(v6.size());

    for (auto const ep : v4)
    {
        auto const bytes = ep->address().to_v4().to_bytes();
        std::memcpy(p, bytes.data(), bytes.size());
        putU32(p + 4, ep->port());
        p += 8;
    }

    for (auto const ep : v6)
    {
        auto const bytes = ep->address().to_v6().to_bytes();
        std::memcpy(p, bytes.data(), bytes.size());
        putU32(p + 16, ep->port());
        p += 20;
    }

    return datagram;
}

std::optional<std::vector<beast::IP::Endpoint>>
decodePeers(Slice datagram)
{
    if (classify(datagram) != MessageType::peers ||
        datagram.size() < tagSize + 2)
        return std::nullopt;

    auto p = datagram.data() + tagSize;
    std::size_t const n4 = p[0];
    std::size_t const n6 = p[1];
    p += 2;

    if (n4 + n6 > maxAdvertisedPeers ||
        datagram.size() != tagSize + 2 + n4 * 8 + n6 * 20)
        return std::nullopt;

    std::vector<beast::IP::Endpoint> endpoints;
    endpoints.reserve(n4 + n6);

    for (std::size_t i = 0; i < n4; ++i, p += 8)
    {
        auto const port = getU32(p + 4);
        if (port > 0xFFFF)
            return std::nullopt;

        boost::asio::ip::address_v4::bytes_type bytes;
        std::memcpy(bytes.data(), p, bytes.size());
        endpoints.emplace_back(
            boost::asio::ip::address_v4(bytes),
            static_cast<std::uint16_t>(port));
    }

    for (std::size_t i = 0; i < n6; ++i, p += 20)
    {
        auto const port = getU32(p + 16);
        if (port > 0xFFFF)
            return std::nullopt;

        boost::asio::ip::address_v6::bytes_type bytes;
        std::memcpy(bytes.data(), p, bytes.size());
        endpoints.emplace_back(
            boost::asio::ip::address_v6(bytes),
            static_cast<std::uint16_t>(port));
    }

    return endpoints;
}

std::vector<Buffer>
encodeTxn(Slice txn, uint256 const& txid, std::size_t maxPayload)
{
    if (txn.empty() || txn.size() > maxTxnSize || maxPayload == 0)
        return {};

    std::size_t const count = (txn.size() + maxPayload - 1) / maxPayload;
    if (count > maxFragments)
        return {};

    std::vector<Buffer> datagrams;
    datagrams.reserve(count);

    for (std::size_t i = 0; i < count; ++i)
    {
        auto const offset = i * maxPayload;
        auto const size = std::min(maxPayload, txn.size() - offset);

        auto& datagram = datagrams.emplace_back(txnHeaderSize + size);
        auto const p = datagram.data();

        std::memcpy(p, txnTag, tagSize);
        std::memcpy(p + tagSize, txid.data(), txid.size());
        putU32(p + 40, static_cast<std::uint32_t>(txn.size()));
        putU32(p + 44, static_cast<std::uint32_t>(count));
        putU32(p + 48, static_cast<std::uint32_t>(i));
        std::memcpy(p + txnHeaderSize, txn.data() + offset, size);
    }

    return datagrams;
}

std::optional<TxnFragment>
decodeTxnFragment(Slice datagram)
{
    if (classify(datagram) != MessageType::txn ||
        datagram.size() <= txnHeaderSize)
        return std::nullopt;

    auto const p = datagram.data();

    TxnFragment fragment;
    fragment.txid = uint256::fromVoid(p + tagSize);
    fragment.totalSize = getU32(p + 40);
    fragment.count = getU32(p + 44);
    fragment.index = getU32(p + 48);
    fragment.payload =
        Slice(p + txnHeaderSize, datagram.size() - txnHeaderSize);

    if (fragment.totalSize == 0 || fragment.totalSize > maxTxnSize ||
        fragment.count == 0 || fragment.count > maxFragments ||
        fragment.count > fragment.totalSize ||
        fragment.index >= fragment.count ||
        fragment.payload.size() > fragment.totalSize)
        return std::nullopt;

    return fragment;
}

//------------------------------------------------------------------------------

Reassembler::Reassembler(
    std::size_t maxPending,
    std::size_t maxBytes,
    std::chrono::seconds timeout)
    : maxPending_(maxPending), maxBytes_(maxBytes), timeout_(timeout)
{
}

Reassembler::Result
Reassembler::add(
    beast::IP::Endpoint const& source,
    TxnFragment const& fragment,
    clock_type::time_point now)
{
    // Most transactions fit in a single datagram and need no state.
    if (fragment.count == 1)
    {
        if (fragment.payload.size() != fragment.totalSize)
            return {Status::invalid, {}};

        return {Status::complete, Buffer(fragment.payload)};
    }

    expire(now);

    Key const key{fragment.txid, source};
    auto it = partials_.find(key);

    if (it == partials_.end())
    {
        if (partials_.size() >= maxPending_)
            return {Status::overloaded, {}};

        Partial partial;
        partial.started = now;
        partial.totalSize = fragment.totalSize;
        partial.count = fragment.count;
        it = partials_.emplace(key, std::move(partial)).first;
    }
    else if (
        it->second.totalSize != fragment.totalSize ||
        it->second.count != fragment.count)
    {
        erase(it);
        return {Status::invalid, {}};
    }

    auto& partial = it->second;

    // Repeated fragments are ignored
    if (partial.fragments.count(fragment.index) != 0)
        return {Status::pending, {}};

    if (partial.received + fragment.payload.size() > partial.totalSize)
    {
        erase(it);
        return {Status::invalid, {}};
    }

    auto const cost = fragment.payload.size() + fragmentOverhead;
    if (bytes_ + cost > maxBytes_)
    {
        if (partial.fragments.empty())
            erase(it);
        return {Status::overloaded, {}};
    }

    partial.fragments.emplace(fragment.index, Buffer(fragment.payload));
    partial.received += fragment.payload.size();
    partial.cost += cost;
    bytes_ += cost;

    if (partial.fragments.size() < partial.count)
        return {Status::pending, {}};

    if (partial.received != partial.totalSize)
    {
        erase(it);
        return {Status::invalid, {}};
    }

    // All fragments are present and std::map keeps them in index order
    Buffer txn(partial.totalSize);
    auto p = txn.data();
    for (auto const& [index, payload] : partial.fragments)
    {
        std::memcpy(p, payload.data(), payload.size());
        p += payload.size();
    }

    erase(it);
    return {Status::complete, std::move(txn)};
}

void
Reassembler::expire(clock_type::time_point now)
{
    for (auto it = partials_.begin(); it != partials_.end();)
    {
        if (now - it->second.started > timeout_)
            erase(it++);
        else
            ++it;
    }
}

void
Reassembler::erase(Map::iterator it)
{
    bytes_ -= it->second.cost;
    partials_.erase(it);
}

}  // namespace xush
}  // namespace ripple
