//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES WHATSOEVER
    RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
    CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
    CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpld/overlay/Message.h>
#include <xrpld/overlay/detail/ProtocolMessage.h>
#include <xrpld/overlay/detail/TrafficCount.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/ExportShare.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Sign.h>

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/multi_buffer.hpp>

#include <memory>
#include <string>
#include <type_traits>

namespace ripple {
namespace test {

class ExportShareTransport_test : public beast::unit_test::suite
{
    static ExportShare
    makeShare()
    {
        auto const [key, secret] = randomKeyPair(KeyType::secp256k1);
        auto const signature = sign(key, secret, Slice{"export-share", 12});
        return ExportShare{
            ExportShare::currentVersion,
            calcAccountID(key),
            uint256{1},
            4'200'000,
            uint256{2},
            uint256{3},
            17,
            key,
            signature};
    }

    struct Handler
    {
        bool seen = false;
        int shares = 0;

        bool
        compressionEnabled() const
        {
            return false;
        }

        void
        onMessageUnknown(std::uint16_t)
        {
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

        void
        onMessageEnd(
            std::uint16_t,
            std::shared_ptr<::google::protobuf::Message> const&)
        {
        }

        template <class Message>
        void
        onMessage(std::shared_ptr<Message> const& message)
        {
            if constexpr (std::is_same_v<Message, protocol::TMExportShares>)
            {
                seen = true;
                shares = message->shares_size();
            }
        }
    };

    void
    testBatchBounds()
    {
        testcase("ExportShare batch structural bounds");

        auto const encoded = makeShare().serialize();
        protocol::TMExportShares batch;
        batch.add_shares(encoded.data(), encoded.size());

        auto const parsed = detail::parseExportShareBatch(batch);
        BEAST_EXPECT(parsed && parsed->size() == 1);
        BEAST_EXPECT(
            batch.ByteSizeLong() <=
            ExportLimits::maxExportShareRelayMessageBytes);

        protocol::TMExportShares maxBatch;
        for (std::size_t i = 0; i < ExportLimits::maxExportSharesPerRelay; ++i)
            maxBatch.add_shares(encoded.data(), encoded.size());
        auto const payloadBytes =
            encoded.size() * ExportLimits::maxExportSharesPerRelay;
        BEAST_EXPECT(
            payloadBytes <= ExportLimits::maxExportShareRelayPayloadBytes);
        BEAST_EXPECT(maxBatch.ByteSizeLong() > payloadBytes);
        BEAST_EXPECT(
            maxBatch.ByteSizeLong() <=
            ExportLimits::maxExportShareRelayMessageBytes);
        BEAST_EXPECT(detail::parseExportShareBatch(maxBatch));

        protocol::TMExportShares empty;
        BEAST_EXPECT(!detail::parseExportShareBatch(empty));

        protocol::TMExportShares tooMany;
        for (std::size_t i = 0; i <= ExportLimits::maxExportSharesPerRelay; ++i)
            tooMany.add_shares(encoded.data(), encoded.size());
        BEAST_EXPECT(!detail::parseExportShareBatch(tooMany));

        protocol::TMExportShares oversized;
        oversized.add_shares(
            std::string(ExportLimits::maxSerializedExportShareBytes + 1, 'x'));
        BEAST_EXPECT(!detail::parseExportShareBatch(oversized));

        protocol::TMExportShares malformed;
        malformed.add_shares("not-an-export-share");
        BEAST_EXPECT(!detail::parseExportShareBatch(malformed));
    }

    void
    testDispatchAndTraffic()
    {
        testcase("ExportShare protocol dispatch and traffic category");

        auto const encoded = makeShare().serialize();
        protocol::TMExportShares batch;
        batch.add_shares(encoded.data(), encoded.size());

        BEAST_EXPECT(protocol::mtEXPORT_SHARES == 65);
        BEAST_EXPECT(
            protocolMessageName(protocol::mtEXPORT_SHARES) == "export_shares");
        BEAST_EXPECT(
            TrafficCount::categorize(batch, protocol::mtEXPORT_SHARES, true) ==
            TrafficCount::category::export_shares);

        Message message{batch, protocol::mtEXPORT_SHARES};
        auto const& wire = message.getBuffer(compression::Compressed::Off);
        boost::beast::multi_buffer buffers;
        buffers.commit(boost::asio::buffer_copy(
            buffers.prepare(wire.size()), boost::asio::buffer(wire)));

        Handler handler;
        std::size_t hint = 0;
        auto const [consumed, ec] =
            invokeProtocolMessage(buffers.data(), handler, hint);
        BEAST_EXPECT(!ec);
        BEAST_EXPECT(consumed == wire.size());
        BEAST_EXPECT(handler.seen);
        BEAST_EXPECT(handler.shares == 1);
    }

public:
    void
    run() override
    {
        testBatchBounds();
        testDispatchAndTraffic();
    }
};

BEAST_DEFINE_TESTSUITE(ExportShareTransport, overlay, ripple);

}  // namespace test
}  // namespace ripple
