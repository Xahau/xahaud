//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

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

#include <test/jtx.h>
#include <xrpld/rpc/detail/CatalogueStream.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpld/shamap/SHAMapItem.h>
#include <xrpld/shamap/SHAMapTreeNode.h>
#include <xrpl/beast/unit_test.h>

#include <boost/iostreams/device/array.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

namespace ripple {
namespace test {

class CatalogueStream_test : public beast::unit_test::suite
{
    // The deserialize* public API takes `CatalogueInputStream&` which is just
    // `boost::iostreams::filtering_istream`. So no production-code change is
    // needed for fault-injection — we hand-build a byte vector that violates
    // the wire format, push it through an `array_source`, and feed it to
    // deserialize. Each test below targets one error branch in
    // CatalogueStream.cpp.
    //
    // Wire format reminder (from serializeSHAMapToStream):
    //   leaf : [type:u8 ][key:32][size:u32][data:size]
    //   remove: [254 :u8 ][key:32]
    //   end  : [255 :u8 ]

    using Bytes = std::vector<std::uint8_t>;

    static void
    appendByte(Bytes& b, std::uint8_t v)
    {
        b.push_back(v);
    }

    static void
    appendBytes(Bytes& b, void const* p, std::size_t n)
    {
        auto const* src = reinterpret_cast<std::uint8_t const*>(p);
        b.insert(b.end(), src, src + n);
    }

    static void
    appendKey(Bytes& b, std::uint8_t fillByte = 0xAA)
    {
        b.insert(b.end(), 32, fillByte);
    }

    static void
    appendU32(Bytes& b, std::uint32_t v)
    {
        // CatalogueStream writes raw bytes — no endian conversion. Match.
        appendBytes(b, &v, sizeof(v));
    }

    // Build a fresh filtering_istream over `bytes`. The array_source is a
    // value type stored by the filter chain, so `bytes` must outlive it.
    static void
    makeStream(Bytes const& bytes, RPC::CatalogueInputStream& out)
    {
        boost::iostreams::array_source src(
            reinterpret_cast<char const*>(bytes.data()), bytes.size());
        out.push(src);
    }

    // Construct an empty SHAMap of the requested type using the test app's
    // node family. Same pattern used in src/test/consensus/UNLReport_test.cpp.
    static std::shared_ptr<SHAMap>
    makeMap(jtx::Env& env, SHAMapType type)
    {
        return std::make_shared<SHAMap>(type, env.app().getNodeFamily());
    }

public:
    void
    testTruncatedKey()
    {
        testcase("deserialize: truncated read of key");

        jtx::Env env(*this);
        auto map = makeMap(env, SHAMapType::STATE);

        // 1 byte (the node type) and nothing after — read of the 32-byte key
        // hits EOF inside deserializeLeaf, triggers the "stream stopped while
        // trying to read key" branch.
        Bytes b;
        appendByte(b, tnACCOUNT_STATE);

        RPC::CatalogueInputStream stream;
        makeStream(b, stream);

        BEAST_EXPECT(!RPC::deserializeStateMapFromStream(
            *map, stream, hotACCOUNT_NODE, env.app().journal("Test")));
    }

    void
    testTruncatedSize()
    {
        testcase("deserialize: truncated read of size");

        jtx::Env env(*this);
        auto map = makeMap(env, SHAMapType::TRANSACTION);

        // type + key but no 4-byte size — triggers the "stream stopped while
        // trying to read size" branch. Use tx map so the type byte isn't a
        // remove (which would short-circuit before the size read).
        Bytes b;
        appendByte(b, tnTRANSACTION_MD);
        appendKey(b);

        RPC::CatalogueInputStream stream;
        makeStream(b, stream);

        BEAST_EXPECT(!RPC::deserializeTxMapFromStream(
            *map, stream, hotTRANSACTION_NODE, env.app().journal("Test")));
    }

    void
    testSizeTooLarge()
    {
        testcase("deserialize: size > 1 GiB rejected");

        jtx::Env env(*this);
        auto map = makeMap(env, SHAMapType::STATE);

        // type + key + size of 2 GiB — bails before any data read attempt.
        Bytes b;
        appendByte(b, tnACCOUNT_STATE);
        appendKey(b);
        appendU32(b, 2u * 1024u * 1024u * 1024u);

        RPC::CatalogueInputStream stream;
        makeStream(b, stream);

        BEAST_EXPECT(!RPC::deserializeStateMapFromStream(
            *map, stream, hotACCOUNT_NODE, env.app().journal("Test")));
    }

    void
    testTruncatedData()
    {
        testcase("deserialize: truncated read of data payload");

        jtx::Env env(*this);
        auto map = makeMap(env, SHAMapType::STATE);

        // type + key + size=10 + only 5 bytes — hits the "Unexpected EOF
        // while reading data" branch.
        Bytes b;
        appendByte(b, tnACCOUNT_STATE);
        appendKey(b);
        appendU32(b, 10);
        for (int i = 0; i < 5; ++i)
            appendByte(b, 0x42);

        RPC::CatalogueInputStream stream;
        makeStream(b, stream);

        BEAST_EXPECT(!RPC::deserializeStateMapFromStream(
            *map, stream, hotACCOUNT_NODE, env.app().journal("Test")));
    }

    void
    testRemoveDisallowedInTxMap()
    {
        testcase(
            "deserialize: remove sentinel rejected when allowRemoval=false");

        jtx::Env env(*this);
        auto map = makeMap(env, SHAMapType::TRANSACTION);

        // tx maps don't accept tnREMOVE — the deserialize for tx maps calls
        // the inner helper with allowRemoval=false. We send a remove record
        // and expect rejection at "unexpected removal in this map type".
        Bytes b;
        appendByte(b, tnREMOVE);
        appendKey(b);

        RPC::CatalogueInputStream stream;
        makeStream(b, stream);

        BEAST_EXPECT(!RPC::deserializeTxMapFromStream(
            *map, stream, hotTRANSACTION_NODE, env.app().journal("Test")));
    }

    void
    testRemoveOfAbsentKey()
    {
        testcase("deserialize: remove of absent key rejected");

        jtx::Env env(*this);
        auto map = makeMap(env, SHAMapType::STATE);

        // State map allows tnREMOVE, but the key has to be present first.
        // Empty map + remove of any key → "key is already absent" branch.
        Bytes b;
        appendByte(b, tnREMOVE);
        appendKey(b, 0x77);

        RPC::CatalogueInputStream stream;
        makeStream(b, stream);

        BEAST_EXPECT(!RPC::deserializeStateMapFromStream(
            *map, stream, hotACCOUNT_NODE, env.app().journal("Test")));
    }

    void
    testMissingTerminator()
    {
        testcase("deserialize: EOF before terminal sentinel");

        jtx::Env env(*this);
        auto map = makeMap(env, SHAMapType::STATE);

        // One valid leaf, then EOF (no tnTERMINAL). The outer loop exits via
        // stream.eof() with lastParsed = tnACCOUNT_STATE (not tnTERMINAL),
        // so the "Unexpected EOF, terminal node not found" branch fires.
        // Payload must be >= 12 bytes — SHAMapLeafNode asserts that for
        // state-tree leaves.
        Bytes b;
        appendByte(b, tnACCOUNT_STATE);
        appendKey(b, 0x11);
        appendU32(b, 16);
        for (int i = 0; i < 16; ++i)
            appendByte(b, 0xCC);

        RPC::CatalogueInputStream stream;
        makeStream(b, stream);

        BEAST_EXPECT(!RPC::deserializeStateMapFromStream(
            *map, stream, hotACCOUNT_NODE, env.app().journal("Test")));
    }

    void
    testEmptyStreamAcceptsTerminator()
    {
        testcase("deserialize: bare terminator → empty map, success");

        jtx::Env env(*this);
        auto map = makeMap(env, SHAMapType::STATE);

        Bytes b;
        appendByte(b, tnTERMINAL);

        RPC::CatalogueInputStream stream;
        makeStream(b, stream);

        BEAST_EXPECT(RPC::deserializeStateMapFromStream(
            *map, stream, hotACCOUNT_NODE, env.app().journal("Test")));
    }

    void
    testHappyPathRoundTrip()
    {
        testcase("deserialize: valid leaf + terminator → success");

        jtx::Env env(*this);
        auto map = makeMap(env, SHAMapType::STATE);

        // type + key + size=16 + 16 data bytes + terminator. Confirms the
        // happy path through addGiveItem and flushDirty. Size must be >= 12
        // — SHAMapLeafNode asserts that for state-tree leaves.
        Bytes b;
        appendByte(b, tnACCOUNT_STATE);
        appendKey(b, 0x55);
        appendU32(b, 16);
        for (int i = 0; i < 16; ++i)
            appendByte(b, static_cast<std::uint8_t>(i));
        appendByte(b, tnTERMINAL);

        RPC::CatalogueInputStream stream;
        makeStream(b, stream);

        BEAST_EXPECT(RPC::deserializeStateMapFromStream(
            *map, stream, hotACCOUNT_NODE, env.app().journal("Test")));

        uint256 key;
        std::memset(key.data(), 0x55, 32);
        BEAST_EXPECT(map->hasItem(key));
    }

    void
    run() override
    {
        testTruncatedKey();
        testTruncatedSize();
        testSizeTooLarge();
        testTruncatedData();
        testRemoveDisallowedInTxMap();
        testRemoveOfAbsentKey();
        testMissingTerminator();
        testEmptyStreamAcceptsTerminator();
        testHappyPathRoundTrip();
    }
};

BEAST_DEFINE_TESTSUITE(CatalogueStream, rpc, ripple);

}  // namespace test
}  // namespace ripple
