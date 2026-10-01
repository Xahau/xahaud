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

#ifndef RIPPLE_OVERLAY_XUSH_H_INCLUDED
#define RIPPLE_OVERLAY_XUSH_H_INCLUDED

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/net/IPEndpoint.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace ripple {

/** XUSH: the Xahau UDP Superhighway.

    A best-effort UDP transport that runs alongside the peer protocol, on the
    same port number as the peer port, and is used to fan transactions out to
    other highway-enabled servers faster than the peer relay alone. Nothing
    depends on it: every transaction sent over the highway is also relayed
    over the peer protocol.

    Every datagram starts with an eight byte ASCII tag that identifies its
    type. All integers are unsigned and big-endian.

    XUSHPEER advertises other highway endpoints:

        [8]         "XUSHPEER"
        [1]         number of IPv4 endpoints (n4)
        [1]         number of IPv6 endpoints (n6)
        [n4 * 8]    4 byte IPv4 address, 4 byte port
        [n6 * 20]   16 byte IPv6 address, 4 byte port

    XUSHTXNF carries one fragment of a serialized transaction:

        [8]         "XUSHTXNF"
        [32]        transaction ID
        [4]         total size of the serialized transaction
        [4]         number of fragments
        [4]         index of this fragment
        [...]       fragment payload

    The payloads of all fragments, concatenated in index order, form the
    serialized transaction.
*/
namespace xush {

enum class MessageType { unknown, peers, txn };

/** Size of the tag at the start of every datagram. */
std::size_t constexpr tagSize = 8;

/** The largest datagram we send.

    This is the IPv6 minimum MTU (1280) less the IPv6 (40) and UDP (8)
    headers, so that our datagrams are never fragmented at the IP layer,
    which many networks drop.
*/
std::size_t constexpr maxDatagramSize = 1232;

/** The most endpoints in a XUSHPEER datagram. */
std::size_t constexpr maxAdvertisedPeers = 50;

/** Size of the XUSHTXNF header. */
std::size_t constexpr txnHeaderSize = 52;

/** The largest fragment payload we send. */
std::size_t constexpr maxFragmentPayload = maxDatagramSize - txnHeaderSize;

/** The largest serialized transaction carried over the highway. */
std::uint32_t constexpr maxTxnSize = 2 * 1024 * 1024;

/** The most fragments a transaction may be split into. */
std::uint32_t constexpr maxFragments = 2048;

static_assert(
    tagSize + 2 + maxAdvertisedPeers * 20 <= maxDatagramSize,
    "XUSHPEER datagrams must fit in maxDatagramSize");
static_assert(
    (maxTxnSize + maxFragmentPayload - 1) / maxFragmentPayload <= maxFragments,
    "The largest transaction must fit in maxFragments fragments");

/** Number of highway endpoints each relayed transaction is sent to.

    Every server relays a transaction at most once (see HashRouter), so this
    bounds the highway traffic per transaction to roughly this many datagrams
    per server while still reaching every server with high probability.
*/
std::size_t constexpr txnFanout = 16;

/** Number of highway endpoints we send a XUSHPEER datagram to each second. */
std::size_t constexpr advertFanout = 4;

/** Identify the type of a datagram from its tag. */
MessageType
classify(Slice datagram);

/** Build a XUSHPEER datagram.

    At most maxAdvertisedPeers endpoints are included.
*/
Buffer
encodePeers(std::vector<beast::IP::Endpoint> const& endpoints);

/** Parse a XUSHPEER datagram.

    @return The advertised endpoints, or nothing if the datagram is malformed.
*/
std::optional<std::vector<beast::IP::Endpoint>>
decodePeers(Slice datagram);

/** A parsed XUSHTXNF datagram. */
struct TxnFragment
{
    uint256 txid;
    std::uint32_t totalSize = 0;
    std::uint32_t count = 0;
    std::uint32_t index = 0;

    /** Refers to the memory of the parsed datagram. */
    Slice payload;
};

/** Split a serialized transaction into XUSHTXNF datagrams.

    @return The datagrams, or nothing if the transaction is empty or larger
            than maxTxnSize.
*/
std::vector<Buffer>
encodeTxn(
    Slice txn,
    uint256 const& txid,
    std::size_t maxPayload = maxFragmentPayload);

/** Parse a XUSHTXNF datagram.

    @return The fragment, or nothing if the datagram is malformed.
*/
std::optional<TxnFragment>
decodeTxnFragment(Slice datagram);

/** Rebuilds serialized transactions from XUSHTXNF fragments.

    Partial transactions are tracked per (transaction ID, sender), so that a
    sender can't corrupt a transaction being rebuilt from another sender's
    fragments. Memory use is bounded by the number of partial transactions,
    the number of bytes buffered and a timeout.

    This class is not thread-safe.
*/
class Reassembler
{
public:
    using clock_type = std::chrono::steady_clock;

    enum class Status {
        /** The fragment was accepted; the transaction is incomplete. */
        pending,

        /** The transaction is complete. */
        complete,

        /** The fragment is inconsistent; the partial transaction was
            discarded. */
        invalid,

        /** The fragment was dropped because a limit was reached. */
        overloaded
    };

    struct Result
    {
        Status status;

        /** The serialized transaction, if status is complete. */
        Buffer txn;
    };

    static std::size_t constexpr defaultMaxPending = 256;
    static std::size_t constexpr defaultMaxBytes = 32 * 1024 * 1024;
    static constexpr std::chrono::seconds defaultTimeout{30};

    /** Bookkeeping cost charged against the byte limit for each buffered
        fragment, so that many tiny fragments can't exhaust memory. */
    static std::size_t constexpr fragmentOverhead = 64;

    explicit Reassembler(
        std::size_t maxPending = defaultMaxPending,
        std::size_t maxBytes = defaultMaxBytes,
        std::chrono::seconds timeout = defaultTimeout);

    Result
    add(beast::IP::Endpoint const& source,
        TxnFragment const& fragment,
        clock_type::time_point now);

    /** Number of partial transactions. */
    std::size_t
    pending() const
    {
        return partials_.size();
    }

    /** Bytes charged against the byte limit. */
    std::size_t
    bytes() const
    {
        return bytes_;
    }

private:
    struct Partial
    {
        clock_type::time_point started;
        std::uint32_t totalSize = 0;
        std::uint32_t count = 0;
        std::size_t received = 0;
        std::size_t cost = 0;
        std::map<std::uint32_t, Buffer> fragments;
    };

    using Key = std::pair<uint256, beast::IP::Endpoint>;
    using Map = std::map<Key, Partial>;

    void
    expire(clock_type::time_point now);

    void
    erase(Map::iterator it);

    std::size_t const maxPending_;
    std::size_t const maxBytes_;
    std::chrono::seconds const timeout_;
    Map partials_;
    std::size_t bytes_ = 0;
};

}  // namespace xush
}  // namespace ripple

#endif
