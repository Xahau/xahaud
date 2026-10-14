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
#include <xrpl/beast/unit_test.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace ripple {
namespace test {

class XUSH_test : public beast::unit_test::suite
{
    using Status = xush::Reassembler::Status;

    static beast::IP::Endpoint
    ep(std::string const& s)
    {
        return beast::IP::Endpoint::from_string(s);
    }

    static Slice
    slice(Buffer const& b)
    {
        return Slice(b.data(), b.size());
    }

    static Buffer
    makeTxn(std::size_t size)
    {
        Buffer b(size);
        for (std::size_t i = 0; i < size; ++i)
            b.data()[i] = static_cast<std::uint8_t>(i * 31 + 7);
        return b;
    }

    static void
    putU32(std::uint8_t* p, std::uint32_t v)
    {
        p[0] = static_cast<std::uint8_t>(v >> 24);
        p[1] = static_cast<std::uint8_t>(v >> 16);
        p[2] = static_cast<std::uint8_t>(v >> 8);
        p[3] = static_cast<std::uint8_t>(v);
    }

    // A XUSHTXNF datagram with an arbitrary (possibly invalid) header
    static Buffer
    rawFragment(
        uint256 const& txid,
        std::uint32_t totalSize,
        std::uint32_t count,
        std::uint32_t index,
        std::size_t payloadSize)
    {
        Buffer b(xush::txnHeaderSize + payloadSize);
        std::memset(b.data(), 0xAB, b.size());
        std::memcpy(b.data(), "XUSHTXNF", xush::tagSize);
        std::memcpy(b.data() + xush::tagSize, txid.data(), txid.size());
        putU32(b.data() + 40, totalSize);
        putU32(b.data() + 44, count);
        putU32(b.data() + 48, index);
        return b;
    }

    // A XUSHPEER datagram with arbitrary (possibly invalid) counts
    static Buffer
    rawPeers(std::uint8_t n4, std::uint8_t n6, std::size_t bodySize)
    {
        Buffer b(xush::tagSize + 2 + bodySize);
        std::memset(b.data(), 0, b.size());
        std::memcpy(b.data(), "XUSHPEER", xush::tagSize);
        b.data()[8] = n4;
        b.data()[9] = n6;
        return b;
    }

    void
    testClassify()
    {
        testcase("classify");
        using namespace xush;

        auto type = [](std::string const& s) { return classify(makeSlice(s)); };

        BEAST_EXPECT(type("XUSHPEER") == MessageType::peers);
        BEAST_EXPECT(type("XUSHPEER and more") == MessageType::peers);
        BEAST_EXPECT(type("XUSHTXNF") == MessageType::txn);
        BEAST_EXPECT(type("XUSHPING") == MessageType::unknown);
        BEAST_EXPECT(type("XUSHPEE") == MessageType::unknown);
        BEAST_EXPECT(type("") == MessageType::unknown);
        BEAST_EXPECT(type("{\"command\":\"ping\"}") == MessageType::unknown);
    }

    void
    testPeers()
    {
        testcase("XUSHPEER");
        using namespace xush;

        std::vector<beast::IP::Endpoint> const endpoints{
            ep("1.2.3.4:51235"), ep("[2001:db8::1]:21337"), ep("5.6.7.8:1")};

        auto const datagram = encodePeers(endpoints);
        BEAST_EXPECT(datagram.size() == tagSize + 2 + 2 * 8 + 20);
        BEAST_EXPECT(classify(slice(datagram)) == MessageType::peers);

        if (auto const decoded = decodePeers(slice(datagram));
            BEAST_EXPECT(decoded))
        {
            BEAST_EXPECT(
                std::set<beast::IP::Endpoint>(
                    decoded->begin(), decoded->end()) ==
                std::set<beast::IP::Endpoint>(
                    endpoints.begin(), endpoints.end()));
        }

        // An empty advertisement is valid
        if (auto const decoded = decodePeers(slice(encodePeers({})));
            BEAST_EXPECT(decoded))
            BEAST_EXPECT(decoded->empty());

        // At most maxAdvertisedPeers are advertised, and the result fits in
        // a single datagram even if they are all IPv6
        {
            std::vector<beast::IP::Endpoint> many;
            for (std::uint16_t i = 1; i <= maxAdvertisedPeers + 10; ++i)
                many.emplace_back(
                    boost::asio::ip::make_address("2001:db8::1"), i);

            auto const big = encodePeers(many);
            BEAST_EXPECT(big.size() <= maxDatagramSize);

            auto const decoded = decodePeers(slice(big));
            BEAST_EXPECT(decoded && decoded->size() == maxAdvertisedPeers);
        }

        // Truncated or padded
        BEAST_EXPECT(!decodePeers(Slice(datagram.data(), datagram.size() - 1)));
        {
            Buffer padded(datagram.size() + 1);
            std::memcpy(padded.data(), datagram.data(), datagram.size());
            BEAST_EXPECT(!decodePeers(slice(padded)));
        }
        BEAST_EXPECT(!decodePeers(Slice(datagram.data(), tagSize + 1)));

        // Too many endpoints, even with a consistent size
        BEAST_EXPECT(!decodePeers(slice(rawPeers(
            maxAdvertisedPeers + 1, 0, (maxAdvertisedPeers + 1) * 8))));
        BEAST_EXPECT(decodePeers(
            slice(rawPeers(maxAdvertisedPeers, 0, maxAdvertisedPeers * 8))));

        // A port that doesn't fit in 16 bits
        {
            auto bad = rawPeers(1, 0, 8);
            putU32(bad.data() + tagSize + 2 + 4, 0x10000);
            BEAST_EXPECT(!decodePeers(slice(bad)));
            putU32(bad.data() + tagSize + 2 + 4, 0xFFFF);
            BEAST_EXPECT(decodePeers(slice(bad)));
        }

        // Wrong tag
        BEAST_EXPECT(!decodePeers(slice(rawFragment(uint256{1}, 1, 1, 0, 1))));
    }

    void
    testTxnEncoding()
    {
        testcase("XUSHTXNF encoding");
        using namespace xush;

        uint256 const txid{42};

        // A typical transaction fits in a single datagram
        {
            auto const txn = makeTxn(200);
            auto const datagrams = encodeTxn(slice(txn), txid);
            BEAST_EXPECT(datagrams.size() == 1);
            BEAST_EXPECT(datagrams[0].size() == txnHeaderSize + 200);

            auto const fragment = decodeTxnFragment(slice(datagrams[0]));
            if (BEAST_EXPECT(fragment))
            {
                BEAST_EXPECT(fragment->txid == txid);
                BEAST_EXPECT(fragment->totalSize == 200);
                BEAST_EXPECT(fragment->count == 1);
                BEAST_EXPECT(fragment->index == 0);
                BEAST_EXPECT(fragment->payload == slice(txn));
            }
        }

        // Larger transactions are split, and no datagram is too large
        for (auto const size :
             {maxFragmentPayload,
              maxFragmentPayload + 1,
              3 * maxFragmentPayload,
              3 * maxFragmentPayload + 17})
        {
            auto const txn = makeTxn(size);
            auto const datagrams = encodeTxn(slice(txn), txid);
            auto const count =
                (size + maxFragmentPayload - 1) / maxFragmentPayload;
            BEAST_EXPECT(datagrams.size() == count);

            std::size_t total = 0;
            for (std::size_t i = 0; i < datagrams.size(); ++i)
            {
                BEAST_EXPECT(datagrams[i].size() <= maxDatagramSize);
                auto const fragment = decodeTxnFragment(slice(datagrams[i]));
                if (!BEAST_EXPECT(fragment))
                    continue;
                BEAST_EXPECT(fragment->index == i);
                BEAST_EXPECT(fragment->count == count);
                BEAST_EXPECT(fragment->totalSize == size);
                BEAST_EXPECT(
                    fragment->payload ==
                    Slice(
                        txn.data() + i * maxFragmentPayload,
                        fragment->payload.size()));
                total += fragment->payload.size();
            }
            BEAST_EXPECT(total == size);
        }

        // Limits
        BEAST_EXPECT(encodeTxn(Slice{}, txid).empty());
        BEAST_EXPECT(encodeTxn(slice(Buffer(maxTxnSize + 1)), txid).empty());
        {
            auto const datagrams = encodeTxn(slice(Buffer(maxTxnSize)), txid);
            BEAST_EXPECT(!datagrams.empty());
            BEAST_EXPECT(datagrams.size() <= maxFragments);
        }
    }

    void
    testTxnDecoding()
    {
        testcase("XUSHTXNF decoding");
        using namespace xush;

        uint256 const id{7};
        auto decode = [](Buffer const& b) {
            return decodeTxnFragment(slice(b)).has_value();
        };

        BEAST_EXPECT(decode(rawFragment(id, 10, 2, 1, 5)));
        BEAST_EXPECT(decode(rawFragment(id, 10, 1, 0, 10)));
        BEAST_EXPECT(decode(rawFragment(id, maxTxnSize, maxFragments, 0, 1)));

        // No payload
        BEAST_EXPECT(!decode(rawFragment(id, 10, 1, 0, 0)));
        // Bad total size
        BEAST_EXPECT(!decode(rawFragment(id, 0, 1, 0, 1)));
        BEAST_EXPECT(!decode(rawFragment(id, maxTxnSize + 1, 2, 0, 1)));
        // Bad fragment count
        BEAST_EXPECT(!decode(rawFragment(id, 10, 0, 0, 1)));
        BEAST_EXPECT(!decode(rawFragment(id, 10, 11, 0, 1)));
        BEAST_EXPECT(
            !decode(rawFragment(id, maxTxnSize, maxFragments + 1, 0, 1)));
        // Bad index
        BEAST_EXPECT(!decode(rawFragment(id, 10, 2, 2, 1)));
        // Payload larger than the transaction
        BEAST_EXPECT(!decode(rawFragment(id, 10, 2, 0, 11)));
        // Wrong tag
        BEAST_EXPECT(!decodeTxnFragment(slice(encodePeers({}))));
    }

    void
    testReassembly()
    {
        testcase("reassembly");
        using namespace xush;
        using namespace std::chrono_literals;

        auto const now = Reassembler::clock_type::now();
        auto const alice = ep("1.2.3.4:5000");
        auto const bob = ep("5.6.7.8:5000");
        auto const carol = ep("9.9.9.9:5000");

        uint256 const id1{1};
        uint256 const id2{2};

        auto const txn1 = makeTxn(2 * maxFragmentPayload + 100);
        auto const txn2 = makeTxn(2 * maxFragmentPayload + 200);
        auto const d1 = encodeTxn(slice(txn1), id1);
        auto const d2 = encodeTxn(slice(txn2), id2);
        BEAST_EXPECT(d1.size() == 3 && d2.size() == 3);

        auto f1 = [&](std::size_t i) {
            return *decodeTxnFragment(slice(d1[i]));
        };
        auto f2 = [&](std::size_t i) {
            return *decodeTxnFragment(slice(d2[i]));
        };

        // Out of order, with a repeated fragment
        {
            Reassembler r;
            BEAST_EXPECT(r.add(alice, f1(2), now).status == Status::pending);
            BEAST_EXPECT(r.add(alice, f1(0), now).status == Status::pending);
            BEAST_EXPECT(r.add(alice, f1(0), now).status == Status::pending);
            BEAST_EXPECT(r.pending() == 1);

            auto const result = r.add(alice, f1(1), now);
            BEAST_EXPECT(result.status == Status::complete);
            BEAST_EXPECT(result.txn == txn1);
            BEAST_EXPECT(r.pending() == 0);
            BEAST_EXPECT(r.bytes() == 0);
        }

        // A transaction in a single datagram needs no state
        {
            Reassembler r;
            auto const small = makeTxn(100);
            auto const d = encodeTxn(slice(small), id2);
            auto const result =
                r.add(alice, *decodeTxnFragment(slice(d[0])), now);
            BEAST_EXPECT(result.status == Status::complete);
            BEAST_EXPECT(result.txn == small);
            BEAST_EXPECT(r.pending() == 0);

            // ... but must be consistent
            auto bad = *decodeTxnFragment(slice(d[0]));
            bad.totalSize += 1;
            BEAST_EXPECT(r.add(alice, bad, now).status == Status::invalid);
        }

        // Senders are kept apart
        {
            Reassembler r;
            BEAST_EXPECT(r.add(alice, f1(0), now).status == Status::pending);
            BEAST_EXPECT(r.add(alice, f1(1), now).status == Status::pending);
            BEAST_EXPECT(r.add(bob, f1(2), now).status == Status::pending);
            BEAST_EXPECT(r.pending() == 2);
            BEAST_EXPECT(r.add(alice, f1(2), now).status == Status::complete);
            BEAST_EXPECT(r.pending() == 1);
        }

        // A header that disagrees with earlier fragments
        {
            Reassembler r;
            BEAST_EXPECT(r.add(alice, f1(0), now).status == Status::pending);
            auto bad = f1(1);
            bad.count += 1;
            BEAST_EXPECT(r.add(alice, bad, now).status == Status::invalid);
            BEAST_EXPECT(r.pending() == 0);
            BEAST_EXPECT(r.bytes() == 0);
        }

        // More bytes than the advertised total size
        {
            Reassembler r;
            auto first = f1(0);
            first.count = 2;
            first.totalSize = maxFragmentPayload + 10;
            auto second = f1(1);
            second.count = 2;
            second.totalSize = first.totalSize;
            BEAST_EXPECT(r.add(alice, first, now).status == Status::pending);
            BEAST_EXPECT(r.add(alice, second, now).status == Status::invalid);
            BEAST_EXPECT(r.pending() == 0);
        }

        // Every fragment present but fewer bytes than the total size
        {
            Reassembler r;
            auto first = f1(0);
            first.count = 2;
            first.totalSize = 2 * maxFragmentPayload + 5;
            auto second = f1(1);
            second.count = 2;
            second.totalSize = first.totalSize;
            BEAST_EXPECT(r.add(alice, first, now).status == Status::pending);
            BEAST_EXPECT(r.add(alice, second, now).status == Status::invalid);
            BEAST_EXPECT(r.pending() == 0);
        }

        // Partial transactions time out
        {
            Reassembler r(
                Reassembler::defaultMaxPending,
                Reassembler::defaultMaxBytes,
                30s);
            BEAST_EXPECT(r.add(alice, f1(0), now).status == Status::pending);
            BEAST_EXPECT(r.add(alice, f1(1), now).status == Status::pending);

            // Not yet
            BEAST_EXPECT(
                r.add(bob, f2(0), now + 30s).status == Status::pending);
            BEAST_EXPECT(r.pending() == 2);

            // Now: the first transaction has to start over
            BEAST_EXPECT(
                r.add(bob, f2(1), now + 31s).status == Status::pending);
            BEAST_EXPECT(r.pending() == 1);
            BEAST_EXPECT(
                r.add(alice, f1(2), now + 31s).status == Status::pending);
            BEAST_EXPECT(r.pending() == 2);
        }

        // The number of partial transactions is limited
        {
            Reassembler r(2, Reassembler::defaultMaxBytes, 30s);
            BEAST_EXPECT(r.add(alice, f1(0), now).status == Status::pending);
            BEAST_EXPECT(r.add(bob, f1(0), now).status == Status::pending);
            BEAST_EXPECT(r.add(carol, f1(0), now).status == Status::overloaded);
            BEAST_EXPECT(r.pending() == 2);

            // Fragments of transactions already being rebuilt are accepted
            BEAST_EXPECT(r.add(alice, f1(1), now).status == Status::pending);
            BEAST_EXPECT(r.add(alice, f1(2), now).status == Status::complete);
            BEAST_EXPECT(r.add(carol, f1(0), now).status == Status::pending);
        }

        // The number of bytes buffered is limited
        {
            Reassembler r(
                Reassembler::defaultMaxPending,
                maxFragmentPayload + Reassembler::fragmentOverhead,
                30s);
            BEAST_EXPECT(r.add(alice, f1(0), now).status == Status::pending);
            BEAST_EXPECT(r.add(alice, f1(1), now).status == Status::overloaded);
            BEAST_EXPECT(r.add(bob, f1(0), now).status == Status::overloaded);
            BEAST_EXPECT(r.pending() == 1);
            BEAST_EXPECT(
                r.bytes() ==
                maxFragmentPayload + Reassembler::fragmentOverhead);
        }
    }

public:
    void
    run() override
    {
        testClassify();
        testPeers();
        testTxnEncoding();
        testTxnDecoding();
        testReassembly();
    }
};

BEAST_DEFINE_TESTSUITE(XUSH, overlay, ripple);

}  // namespace test
}  // namespace ripple
