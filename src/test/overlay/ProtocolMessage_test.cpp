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

#include <xrpld/overlay/Message.h>
#include <xrpld/overlay/detail/ProtocolMessage.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/messages.h>

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/multi_buffer.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ripple {
namespace test {

/** Receive-side framing tests for invokeProtocolMessage.

    PeerImp::onReadMessage hands its entire read buffer to
    invokeProtocolMessage, which must decode exactly one frame and report
    exactly that frame's size as consumed. A single read routinely contains
    several frames, so the payload parser must never look past the payload
    size declared in the frame header.
*/
class ProtocolMessage_test : public beast::unit_test::suite
{
    using Compressed = compression::Compressed;

    /** Minimal stand-in for the PeerImp handler interface. */
    struct Handler
    {
        bool compression = false;
        std::vector<std::shared_ptr<::google::protobuf::Message>> received;
        std::vector<std::uint16_t> types;

        bool
        compressionEnabled() const
        {
            return compression;
        }

        void
        onMessageBegin(
            std::uint16_t,
            std::shared_ptr<::google::protobuf::Message> const&,
            std::size_t,
            std::size_t,
            bool)
        {
        }

        template <class T>
        void
        onMessage(std::shared_ptr<T> const& m)
        {
            received.push_back(m);
        }

        void
        onMessageEnd(
            std::uint16_t type,
            std::shared_ptr<::google::protobuf::Message> const&)
        {
            types.push_back(type);
        }

        void
        onMessageUnknown(std::uint16_t type)
        {
            types.push_back(type);
        }
    };

    /** Loads wire bytes into a multi_buffer as one or two committed
        segments, split at `split` (no split if split is 0 or past the end).
    */
    static void
    load(
        boost::beast::multi_buffer& rb,
        std::vector<std::uint8_t> const& wire,
        std::size_t split = 0)
    {
        auto commit = [&rb](std::uint8_t const* p, std::size_t n) {
            if (n != 0)
                rb.commit(boost::asio::buffer_copy(
                    rb.prepare(n), boost::asio::buffer(p, n)));
        };

        if (split == 0 || split >= wire.size())
        {
            commit(wire.data(), wire.size());
            return;
        }
        commit(wire.data(), split);
        commit(wire.data() + split, wire.size() - split);
    }

    /** Mirrors the consume loop in PeerImp::onReadMessage. */
    static boost::system::error_code
    drain(boost::beast::multi_buffer& rb, Handler& handler)
    {
        std::size_t hint = 0;
        while (rb.size() > 0)
        {
            auto const [consumed, ec] =
                invokeProtocolMessage(rb.data(), handler, hint);
            if (ec)
                return ec;
            if (consumed == 0)
                break;
            rb.consume(consumed);
        }
        return {};
    }

    static std::vector<std::uint8_t>
    frame(
        ::google::protobuf::Message const& m,
        protocol::MessageType type,
        Compressed compressed = Compressed::Off)
    {
        Message msg(m, type);
        return msg.getBuffer(compressed);
    }

    static protocol::TMPing
    makePing(protocol::TMPing::pingType type, std::uint32_t seq)
    {
        protocol::TMPing ping;
        ping.set_type(type);
        ping.set_seq(seq);
        return ping;
    }

    template <class T>
    std::shared_ptr<T>
    receivedAs(Handler const& h, std::size_t i)
    {
        if (!BEAST_EXPECT(i < h.received.size()))
            return {};
        auto p = std::dynamic_pointer_cast<T>(h.received[i]);
        BEAST_EXPECT(p);
        return p;
    }

    void
    testCoalescedUncompressed()
    {
        testcase("Coalesced uncompressed frames");

        auto const first = makePing(protocol::TMPing::ptPING, 7);
        auto second = makePing(protocol::TMPing::ptPONG, 99);
        second.set_nettime(5);

        auto wire = frame(first, protocol::mtPING);
        auto const firstSize = wire.size();
        auto const tail = frame(second, protocol::mtPING);
        wire.insert(wire.end(), tail.begin(), tail.end());

        // Both frames delivered by a single read, with every possible
        // segmentation of the read buffer (0 means one contiguous segment).
        for (std::size_t split = 0; split < wire.size(); ++split)
        {
            boost::beast::multi_buffer rb;
            load(rb, wire, split);

            Handler h;
            auto const ec = drain(rb, h);

            BEAST_EXPECTS(
                !ec, "split " + std::to_string(split) + ": " + ec.message());
            BEAST_EXPECT(rb.size() == 0);
            if (!BEAST_EXPECT(h.received.size() == 2))
                continue;

            auto const a = receivedAs<protocol::TMPing>(h, 0);
            auto const b = receivedAs<protocol::TMPing>(h, 1);
            if (!a || !b)
                continue;
            BEAST_EXPECT(a->type() == protocol::TMPing::ptPING);
            BEAST_EXPECT(a->seq() == 7);
            BEAST_EXPECT(!a->has_nettime());
            BEAST_EXPECT(b->type() == protocol::TMPing::ptPONG);
            BEAST_EXPECT(b->seq() == 99);
            BEAST_EXPECT(b->nettime() == 5);
        }

        // The first frame on its own, followed by a partial second frame,
        // is delivered alone and the remainder is left for the next read.
        {
            boost::beast::multi_buffer rb;
            std::vector<std::uint8_t> partial(
                wire.begin(), wire.begin() + firstSize + 3);
            load(rb, partial);

            Handler h;
            BEAST_EXPECT(!drain(rb, h));
            BEAST_EXPECT(h.received.size() == 1);
            BEAST_EXPECT(rb.size() == 3);
        }
    }

    void
    testUncompressedThenCompressed()
    {
        testcase("Uncompressed frame followed by compressed frame");

        auto const ping = makePing(protocol::TMPing::ptPING, 1234);

        protocol::TMTransaction tx;
        tx.set_rawtransaction(std::string(4096, 'x'));
        tx.set_status(protocol::tsNEW);

        auto wire = frame(ping, protocol::mtPING);
        auto const txFrame = frame(tx, protocol::mtTRANSACTION, Compressed::On);
        // The second frame must really be compressed for this test to mean
        // anything: its header is what the first parse used to run into.
        if (!BEAST_EXPECT(!txFrame.empty() && (txFrame[0] & 0x80)))
            return;
        wire.insert(wire.end(), txFrame.begin(), txFrame.end());

        for (std::size_t split = 0; split < wire.size(); split += 7)
        {
            boost::beast::multi_buffer rb;
            load(rb, wire, split);

            Handler h;
            h.compression = true;
            auto const ec = drain(rb, h);

            BEAST_EXPECTS(
                !ec, "split " + std::to_string(split) + ": " + ec.message());
            BEAST_EXPECT(rb.size() == 0);
            if (!BEAST_EXPECT(h.received.size() == 2))
                continue;

            auto const p = receivedAs<protocol::TMPing>(h, 0);
            auto const t = receivedAs<protocol::TMTransaction>(h, 1);
            if (!p || !t)
                continue;
            BEAST_EXPECT(p->type() == protocol::TMPing::ptPING);
            BEAST_EXPECT(p->seq() == 1234);
            BEAST_EXPECT(t->rawtransaction() == tx.rawtransaction());
            BEAST_EXPECT(t->status() == protocol::tsNEW);
        }
    }

    void
    testDeclaredSizeIsAuthoritative()
    {
        testcase("Declared payload size bounds the parse");

        auto const ping = makePing(protocol::TMPing::ptPING, 7);
        auto wire = frame(ping, protocol::mtPING);

        // Shrink the declared payload size by one byte without removing the
        // byte from the stream. The declared payload alone is a truncated
        // TMPing, so the frame must be rejected rather than completed from
        // bytes that lie beyond it.
        std::uint32_t declared = (std::uint32_t(wire[0]) << 24) |
            (std::uint32_t(wire[1]) << 16) | (std::uint32_t(wire[2]) << 8) |
            std::uint32_t(wire[3]);
        if (!BEAST_EXPECT(declared > 0))
            return;
        --declared;
        wire[0] = static_cast<std::uint8_t>(declared >> 24);
        wire[1] = static_cast<std::uint8_t>(declared >> 16);
        wire[2] = static_cast<std::uint8_t>(declared >> 8);
        wire[3] = static_cast<std::uint8_t>(declared);

        boost::beast::multi_buffer rb;
        load(rb, wire);

        Handler h;
        auto const ec = drain(rb, h);
        BEAST_EXPECT(ec == boost::system::errc::bad_message);
        BEAST_EXPECT(h.received.empty());
    }

public:
    void
    run() override
    {
        testCoalescedUncompressed();
        testUncompressedThenCompressed();
        testDeclaredSizeIsAuthoritative();
    }
};

BEAST_DEFINE_TESTSUITE(ProtocolMessage, overlay, ripple);

}  // namespace test
}  // namespace ripple
