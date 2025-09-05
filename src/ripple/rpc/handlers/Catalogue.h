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

#ifndef RIPPLE_RPC_HANDLERS_CATALOGUE_H_INCLUDED
#define RIPPLE_RPC_HANDLERS_CATALOGUE_H_INCLUDED

#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <shared_mutex>
#include <string>

namespace ripple {

#define CATL 0x4C544143UL /*"CATL" in LE*/

// Version constants
static constexpr uint16_t CATALOGUE_VERSION = 1;
static constexpr uint16_t CATALOGUE_VERSION_MASK =
    0x00FF;  // Lower 8 bits for version
static constexpr uint16_t CATALOGUE_COMPRESS_LEVEL_MASK =
    0x0F00;  // Bits 8-11: compression level
[[maybe_unused]] static constexpr uint16_t CATALOGUE_RESERVED_MASK =
    0xF000;  // Bits 12-15: reserved

// Catalogue file header structure
#pragma pack(push, 1)  // pack the struct tightly
struct CATLHeader
{
    uint32_t magic = CATL;
    uint32_t min_ledger;
    uint32_t max_ledger;
    uint16_t version;
    uint16_t network_id;
    uint64_t filesize = 0;  // Total size of the file including header
    std::array<uint8_t, 64> hash = {};  // SHA-512 hash, initially set to zeros
};
#pragma pack(pop)

// Job type for catalogue operations
enum class CatalogueJobType { CREATE, LOAD };

// Runtime status for catalogue operations
struct CatalogueRunStatus
{
    bool isRunning = false;
    std::chrono::system_clock::time_point started;
    uint32_t minLedger;
    uint32_t maxLedger;
    uint32_t ledgerUpto;
    CatalogueJobType jobType;
    std::string filename;
    uint8_t compressionLevel = 0;
    std::string hash;                           // Hex-encoded hash
    uint64_t filesize = 0;                      // File size in bytes
    std::string fileSizeEstimated = "unknown";  // Estimated file size
    int loadThreads = 0;                        // Number of threads for loading
    std::atomic<uint64_t> fileBytesProcessed{
        0};  // Bytes read from decompressed stream
};

// Global status for catalogue operations
extern std::shared_mutex catalogueStatusMutex;
extern CatalogueRunStatus catalogueRunStatus;

// Macro to simplify status updates
#define UPDATE_CATALOGUE_STATUS(field, value)                                \
    {                                                                        \
        std::unique_lock<std::shared_mutex> writeLock(catalogueStatusMutex); \
        catalogueRunStatus.field = value;                                    \
    }

// Output byte counter filter for compression streams
class ByteCounterFilter : public boost::iostreams::output_filter
{
private:
    uint64_t bytesWritten_;

public:
    ByteCounterFilter() : bytesWritten_(0)
    {
    }

    template <typename Sink>
    bool
    put(Sink& sink, char c)
    {
        bool result = boost::iostreams::put(sink, c);
        if (result)
            bytesWritten_++;
        return result;
    }

    template <typename Sink>
    std::streamsize
    write(Sink& sink, const char* data, std::streamsize n)
    {
        std::streamsize result = boost::iostreams::write(sink, data, n);
        if (result > 0)
            bytesWritten_ += result;
        return result;
    }

    uint64_t
    getBytesWritten() const
    {
        return bytesWritten_;
    }

    void
    resetCounter()
    {
        bytesWritten_ = 0;
    }
};

// Input byte counter filter for decompression streams
class ByteCounterInputFilter : public boost::iostreams::input_filter
{
private:
    std::atomic<uint64_t>* bytesRead_;

public:
    explicit ByteCounterInputFilter(std::atomic<uint64_t>* counter)
        : bytesRead_(counter)
    {
    }

    template <typename Source>
    int
    get(Source& src)
    {
        int result = boost::iostreams::get(src);
        if (result != EOF && bytesRead_)
            (*bytesRead_)++;
        return result;
    }

    template <typename Source>
    std::streamsize
    read(Source& src, char* data, std::streamsize n)
    {
        std::streamsize result = boost::iostreams::read(src, data, n);
        if (result > 0 && bytesRead_)
            *bytesRead_ += result;
        return result;
    }
};

// Size predictor for catalogue files
class CatalogueSizePredictor
{
private:
    uint32_t minLedger_;
    uint32_t maxLedger_;
    [[maybe_unused]] uint64_t headerSize_;

    uint64_t totalBytesWritten_;
    uint64_t firstLedgerSize_;
    uint64_t processedLedgers_;
    std::deque<uint64_t> recentDeltas_;
    static constexpr size_t MAX_DELTAS = 10;

public:
    CatalogueSizePredictor(
        uint32_t minLedger,
        uint32_t maxLedger,
        uint64_t headerSize);

    void
    addLedger(uint32_t seq, uint64_t bytes);

    uint64_t
    getEstimate() const;

    std::string
    getEstimateHuman() const;
};

// Helper functions for version field manipulation
inline uint8_t
getCatalogueVersion(uint16_t versionField)
{
    return versionField & CATALOGUE_VERSION_MASK;
}

inline uint8_t
getCompressionLevel(uint16_t versionField)
{
    return (versionField & CATALOGUE_COMPRESS_LEVEL_MASK) >> 8;
}

inline bool
isCompressed(uint16_t versionField)
{
    return getCompressionLevel(versionField) > 0;
}

inline uint16_t
makeCatalogueVersionField(uint8_t version, uint8_t compressionLevel = 0)
{
    if (compressionLevel > 9)
        compressionLevel = 9;

    uint16_t result = version & CATALOGUE_VERSION_MASK;
    result |= (compressionLevel << 8);
    return result;
}

// Helper functions
std::string
formatBytesIEC(uint64_t bytes, int precision = 2);

std::string
toHexString(unsigned char const* data, size_t len);

Json::Value
generateStatusJson(bool includeErrorInfo = false);

// RPC handlers
Json::Value
doCatalogueStatus(RPC::JsonContext& context);

Json::Value
doCatalogueCreate(RPC::JsonContext& context);

Json::Value
doCatalogueLoad(RPC::JsonContext& context);

}  // namespace ripple

#endif