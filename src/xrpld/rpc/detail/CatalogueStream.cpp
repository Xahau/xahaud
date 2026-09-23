//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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

#include <xrpld/rpc/detail/CatalogueStream.h>

#include <xrpl/basics/Slice.h>

#include <chrono>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ripple {
namespace RPC {

namespace {

std::size_t
serializeSHAMapToStream(
    SHAMap const& shaMap,
    CatalogueOutputStream& stream,
    SHAMapNodeType nodeType,
    std::optional<std::reference_wrapper<SHAMap const>> baseSHAMap =
        std::nullopt)
{
    using StreamState =
        std::pair<std::uint64_t, std::chrono::steady_clock::time_point>;

    static std::mutex streamStateMutex;
    static std::unordered_map<void*, StreamState> streamBytesWritten;

    constexpr std::uint64_t flushThreshold = 256 * 1024 * 1024;
    std::uint64_t localBytesWritten = 0;

    auto tryFlush = [](auto& s) {
        if constexpr (requires(decltype(s) str) { str.flush(); })
        {
            s.flush();
        }
    };

    {
        std::lock_guard<std::mutex> lock(streamStateMutex);
        auto const now = std::chrono::steady_clock::now();

        if (auto const it =
                streamBytesWritten.find(static_cast<void*>(&stream));
            it != streamBytesWritten.end())
        {
            localBytesWritten = it->second.first;
        }

        for (auto it = streamBytesWritten.begin();
             it != streamBytesWritten.end();)
        {
            if (now - it->second.second > std::chrono::minutes(5))
                it = streamBytesWritten.erase(it);
            else
                ++it;
        }
    }

    auto serializeLeaf = [&stream, &localBytesWritten, &tryFlush](
                             SHAMapItem const& item,
                             SHAMapNodeType type) -> bool {
        stream.write(reinterpret_cast<char const*>(&type), 1);
        localBytesWritten += 1;

        auto const key = item.key();
        stream.write(reinterpret_cast<char const*>(key.data()), 32);
        localBytesWritten += 32;

        auto const data = item.slice();
        std::uint32_t size = data.size();
        stream.write(reinterpret_cast<char const*>(&size), 4);
        localBytesWritten += 4;

        stream.write(reinterpret_cast<char const*>(data.data()), size);
        localBytesWritten += size;

        if (localBytesWritten >= flushThreshold)
        {
            tryFlush(stream);
            localBytesWritten = 0;
        }

        return !stream.fail();
    };

    auto serializeRemovedLeaf =
        [&stream, &localBytesWritten, &tryFlush](uint256 const& key) -> bool {
        auto t = SHAMapNodeType::tnREMOVE;
        stream.write(reinterpret_cast<char const*>(&t), 1);
        localBytesWritten += 1;

        stream.write(reinterpret_cast<char const*>(key.data()), 32);
        localBytesWritten += 32;

        if (localBytesWritten >= flushThreshold)
        {
            tryFlush(stream);
            localBytesWritten = 0;
        }

        return !stream.fail();
    };

    std::size_t nodeCount = 0;

    if (baseSHAMap)
    {
        auto const& baseMap = baseSHAMap->get();

        if (shaMap.getHash() != baseMap.getHash())
        {
            SHAMap::Delta differences;

            if (shaMap.compare(
                    baseMap, differences, std::numeric_limits<int>::max()))
            {
                for (auto const& [key, deltaItem] : differences)
                {
                    auto const& newItem = deltaItem.first;
                    auto const& oldItem = deltaItem.second;

                    if (!oldItem && newItem)
                    {
                        if (serializeLeaf(*newItem, nodeType))
                            ++nodeCount;
                    }
                    else if (oldItem && !newItem)
                    {
                        if (serializeRemovedLeaf(key))
                            ++nodeCount;
                    }
                    else if (
                        oldItem && newItem &&
                        oldItem->slice() != newItem->slice())
                    {
                        if (serializeLeaf(*newItem, nodeType))
                            ++nodeCount;
                    }
                }

                auto t = SHAMapNodeType::tnTERMINAL;
                stream.write(reinterpret_cast<char const*>(&t), 1);
                localBytesWritten += 1;

                if (localBytesWritten >= flushThreshold)
                {
                    tryFlush(stream);
                    localBytesWritten = 0;
                }

                std::lock_guard<std::mutex> lock(streamStateMutex);
                streamBytesWritten[static_cast<void*>(&stream)] = {
                    localBytesWritten, std::chrono::steady_clock::now()};

                return nodeCount;
            }
        }
        else
        {
            return 0;
        }
    }

    for (auto const& item : shaMap)
    {
        if (serializeLeaf(item, nodeType))
            ++nodeCount;
    }

    auto t = SHAMapNodeType::tnTERMINAL;
    stream.write(reinterpret_cast<char const*>(&t), 1);
    localBytesWritten += 1;

    if (localBytesWritten >= flushThreshold)
    {
        tryFlush(stream);
        localBytesWritten = 0;
    }

    {
        std::lock_guard<std::mutex> lock(streamStateMutex);
        streamBytesWritten[static_cast<void*>(&stream)] = {
            localBytesWritten, std::chrono::steady_clock::now()};
    }

    return nodeCount;
}

bool
deserializeSHAMapFromStream(
    SHAMap& shaMap,
    CatalogueInputStream& stream,
    SHAMapNodeType nodeType,
    NodeObjectType flushType,
    bool allowRemoval,
    beast::Journal const& j)
{
    try
    {
        auto deserializeLeaf = [&shaMap, &stream, &j, nodeType, allowRemoval](
                                   SHAMapNodeType& parsedType) -> bool {
            stream.read(reinterpret_cast<char*>(&parsedType), 1);

            if (parsedType == SHAMapNodeType::tnTERMINAL)
                return false;

            uint256 key;
            std::uint32_t size{0};

            stream.read(reinterpret_cast<char*>(key.data()), 32);

            if (stream.fail())
            {
                JLOG(j.error())
                    << "Deserialization: stream stopped unexpectedly "
                    << "while trying to read key of next entry";
                return false;
            }

            if (parsedType == SHAMapNodeType::tnREMOVE)
            {
                if (!allowRemoval)
                {
                    JLOG(j.error())
                        << "Deserialization: unexpected removal in this map "
                           "type";
                    return false;
                }

                if (!shaMap.hasItem(key))
                {
                    JLOG(j.error())
                        << "Deserialization: removal of key " << to_string(key)
                        << " but key is already absent.";
                    return false;
                }

                shaMap.delItem(key);
                return true;
            }

            stream.read(reinterpret_cast<char*>(&size), 4);

            if (stream.fail())
            {
                JLOG(j.error())
                    << "Deserialization: stream stopped unexpectedly while "
                    << "trying to read size of data for key " << to_string(key);
                return false;
            }

            if (size > 1024 * 1024 * 1024)
            {
                JLOG(j.error())
                    << "Deserialization: size of " << to_string(key)
                    << " is suspiciously large (" << size << " bytes), "
                    << "bailing.";
                return false;
            }

            std::vector<std::uint8_t> data(size);
            stream.read(reinterpret_cast<char*>(data.data()), size);

            if (stream.fail())
            {
                JLOG(j.error())
                    << "Deserialization: Unexpected EOF while reading data "
                    << "for " << to_string(key);
                return false;
            }

            auto item = make_shamapitem(key, makeSlice(data));
            if (shaMap.hasItem(key))
                return shaMap.updateGiveItem(nodeType, std::move(item));

            return shaMap.addGiveItem(nodeType, std::move(item));
        };

        auto lastParsed = SHAMapNodeType::tnREMOVE;
        while (!stream.eof() && deserializeLeaf(lastParsed))
            ;

        if (lastParsed != SHAMapNodeType::tnTERMINAL)
        {
            JLOG(j.error())
                << "Deserialization: Unexpected EOF, terminal node not found.";
            return false;
        }

        shaMap.flushDirty(flushType);
        return true;
    }
    catch (std::exception const& e)
    {
        JLOG(j.error()) << "Exception during deserialization: " << e.what();
        return false;
    }
}

}  // namespace

std::size_t
serializeStateMapToStream(
    SHAMap const& stateMap,
    CatalogueOutputStream& stream,
    std::optional<std::reference_wrapper<SHAMap const>> prevStateMap)
{
    return serializeSHAMapToStream(
        stateMap, stream, SHAMapNodeType::tnACCOUNT_STATE, prevStateMap);
}

std::size_t
serializeTxMapToStream(SHAMap const& txMap, CatalogueOutputStream& stream)
{
    return serializeSHAMapToStream(
        txMap, stream, SHAMapNodeType::tnTRANSACTION_MD);
}

bool
deserializeStateMapFromStream(
    SHAMap& stateMap,
    CatalogueInputStream& stream,
    NodeObjectType flushType,
    beast::Journal const& j)
{
    return deserializeSHAMapFromStream(
        stateMap, stream, SHAMapNodeType::tnACCOUNT_STATE, flushType, true, j);
}

bool
deserializeTxMapFromStream(
    SHAMap& txMap,
    CatalogueInputStream& stream,
    NodeObjectType flushType,
    beast::Journal const& j)
{
    return deserializeSHAMapFromStream(
        txMap, stream, SHAMapNodeType::tnTRANSACTION_MD, flushType, false, j);
}

}  // namespace RPC
}  // namespace ripple
