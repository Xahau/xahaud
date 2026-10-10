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

#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/LedgerToJson.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/SHAMapStore.h>
#include <xrpld/app/rdb/backend/SQLiteDatabase.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/ledger/View.h>
#include <xrpld/nodestore/detail/DatabasePinnedImp.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/GRPCHandlers.h>
#include <xrpld/rpc/Role.h>
#include <xrpld/rpc/detail/CatalogueStream.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpld/rpc/detail/Tuning.h>
#include <xrpld/shamap/SHAMapItem.h>
#include <xrpl/basics/Log.h>
#include <xrpl/basics/RangeSet.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/operations.hpp>

#include <chrono>

namespace ripple {

using time_point = NetClock::time_point;
using duration = NetClock::duration;

#define CATL 0x4C544143UL /*"CATL" in LE*/

// Replace the current version constant
static constexpr uint16_t CATALOGUE_VERSION = 1;

// Instead use these definitions
static constexpr uint16_t CATALOGUE_VERSION_MASK =
    0x00FF;  // Lower 8 bits for version
static constexpr uint16_t CATALOGUE_COMPRESS_LEVEL_MASK =
    0x0F00;  // Bits 8-11: compression level
[[maybe_unused]] static constexpr uint16_t CATALOGUE_RESERVED_MASK =
    0xF000;  // Bits 12-15: reserved

std::string
formatBytesIEC(uint64_t bytes, int precision = 2)
{
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    int unit_index = 0;
    auto size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 5)
    {
        size /= 1024.0;
        unit_index++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << size << " "
        << units[unit_index];
    return oss.str();
}

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
{  // 0 = no compression

    // Ensure compression level is within valid range (0-9)
    if (compressionLevel > 9)
        compressionLevel = 9;

    uint16_t result = version & CATALOGUE_VERSION_MASK;
    result |= (compressionLevel << 8);  // Store level in bits 8-11
    return result;
}

// Helper function to convert binary hash to hex string
std::string
toHexString(unsigned char const* data, size_t len)
{
    static char const* hexDigits = "0123456789ABCDEF";
    std::string result;
    result.reserve(2 * len);
    for (size_t i = 0; i < len; ++i)
    {
        unsigned char c = data[i];
        result.push_back(hexDigits[c >> 4]);
        result.push_back(hexDigits[c & 15]);
    }
    return result;
}

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

enum class CatalogueJobType { CREATE, LOAD };

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
};

// Global status for catalogue operations
static std::shared_mutex
    catalogueStatusMutex;  // Protects access to the status object
static CatalogueRunStatus catalogueRunStatus;  // Always in memory

// Macro to simplify common patterns
#define UPDATE_CATALOGUE_STATUS(field, value)                                \
    {                                                                        \
        std::unique_lock<std::shared_mutex> writeLock(catalogueStatusMutex); \
        catalogueRunStatus.field = value;                                    \
    }

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

// Simple size predictor class
class CatalogueSizePredictor
{
private:
    uint32_t minLedger_;
    uint32_t maxLedger_;

    // Keep track of actual bytes
    uint64_t totalBytesWritten_;
    uint64_t firstLedgerSize_;
    uint64_t processedLedgers_;
    std::deque<uint64_t> recentDeltas_;
    static constexpr size_t MAX_DELTAS = 10;

public:
    CatalogueSizePredictor(
        uint32_t minLedger,
        uint32_t maxLedger,
        uint64_t headerSize)
        : minLedger_(minLedger)
        , maxLedger_(maxLedger)
        , totalBytesWritten_(headerSize)
        , firstLedgerSize_(0)
        , processedLedgers_(0)
    {
    }

    void
    addLedger(uint32_t seq, uint64_t bytes)
    {
        totalBytesWritten_ += bytes;
        processedLedgers_++;

        if (seq == minLedger_)
        {
            firstLedgerSize_ = bytes;
        }
        else
        {
            // Track recent deltas
            recentDeltas_.push_back(bytes);
            if (recentDeltas_.size() > MAX_DELTAS)
                recentDeltas_.pop_front();
        }
    }

    // Get current size estimate
    uint64_t
    getEstimate() const
    {
        if (recentDeltas_.empty())
        {
            return totalBytesWritten_;
        }

        uint64_t totalDeltaSize = 0;
        for (auto size : recentDeltas_)
            totalDeltaSize += size;

        uint64_t avgDelta = totalDeltaSize / recentDeltas_.size();

        uint32_t totalLedgers = maxLedger_ - minLedger_ + 1;
        uint32_t remainingLedgers = (totalLedgers >= processedLedgers_)
            ? (totalLedgers - processedLedgers_)
            : 0;

        return totalBytesWritten_ + avgDelta * remainingLedgers;
    }

    std::string
    getEstimateHuman() const
    {
        auto bytes = getEstimate();
        if (bytes == totalBytesWritten_)
            return totalBytesWritten_ == 0
                ? "unknown"
                : formatBytesIEC(totalBytesWritten_) + "+";
        return formatBytesIEC(bytes);
    }
};

// Helper function to generate status JSON
// IMPORTANT: Caller must hold at least a shared (read) lock on
// catalogueStatusMutex before calling this function
inline Json::Value
generateStatusJson(bool includeErrorInfo = false)
{
    Json::Value jvResult;

    if (catalogueRunStatus.isRunning)
    {
        jvResult[jss::job_status] = "job_in_progress";
        jvResult[jss::min_ledger] = catalogueRunStatus.minLedger;
        jvResult[jss::max_ledger] = catalogueRunStatus.maxLedger;
        jvResult[jss::current_ledger] = catalogueRunStatus.ledgerUpto;

        // Calculate percentage complete - FIX: Handle ledgerUpto = 0 case
        // properly
        uint32_t total_ledgers =
            catalogueRunStatus.maxLedger - catalogueRunStatus.minLedger + 1;

        // If ledgerUpto is 0, it means no progress has been made yet
        uint32_t processed_ledgers = (catalogueRunStatus.ledgerUpto == 0)
            ? 0
            : catalogueRunStatus.ledgerUpto - catalogueRunStatus.minLedger + 1;

        if (processed_ledgers > total_ledgers)
            processed_ledgers = total_ledgers;  // Safety check

        int percentage = (total_ledgers > 0)
            ? static_cast<int>((processed_ledgers * 100) / total_ledgers)
            : 0;
        jvResult[jss::percent_complete] = percentage;

        // Calculate elapsed time
        auto now = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           now - catalogueRunStatus.started)
                           .count();
        jvResult[jss::elapsed_seconds] = static_cast<Json::UInt>(elapsed);

        // Calculate estimated time remaining
        if (processed_ledgers > 0 && total_ledgers > processed_ledgers)
        {
            // Calculate rate: ledgers per second
            double ledgers_per_second =
                static_cast<double>(processed_ledgers) / elapsed;

            if (ledgers_per_second > 0)
            {
                // Calculate remaining time in seconds
                uint32_t remaining_ledgers = total_ledgers - processed_ledgers;
                uint64_t estimated_seconds_remaining = static_cast<uint64_t>(
                    remaining_ledgers / ledgers_per_second);

                // Format the time remaining in human-readable form
                std::string time_remaining;
                if (estimated_seconds_remaining > 3600)
                {
                    // Hours and minutes
                    uint64_t hours = estimated_seconds_remaining / 3600;
                    uint64_t minutes =
                        (estimated_seconds_remaining % 3600) / 60;
                    time_remaining = std::to_string(hours) + " hour" +
                        (hours > 1 ? "s" : "") + " " + std::to_string(minutes) +
                        " minute" + (minutes > 1 ? "s" : "");
                }
                else if (estimated_seconds_remaining > 60)
                {
                    // Minutes and seconds
                    uint64_t minutes = estimated_seconds_remaining / 60;
                    uint64_t seconds = estimated_seconds_remaining % 60;
                    time_remaining = std::to_string(minutes) + " minute" +
                        (minutes > 1 ? "s" : "") + " " +
                        std::to_string(seconds) + " second" +
                        (seconds > 1 ? "s" : "");
                }
                else
                {
                    // Just seconds
                    time_remaining =
                        std::to_string(estimated_seconds_remaining) +
                        " second" +
                        (estimated_seconds_remaining > 1 ? "s" : "");
                }
                jvResult[jss::estimated_time_remaining] = time_remaining;
            }
            else
            {
                jvResult[jss::estimated_time_remaining] = "unknown";
            }
        }
        else
        {
            jvResult[jss::estimated_time_remaining] = "unknown";
        }

        // Add start time as ISO 8601 string
        auto time_t_started =
            std::chrono::system_clock::to_time_t(catalogueRunStatus.started);
        std::tm* tm_started = std::gmtime(&time_t_started);
        char time_buffer[30];
        std::strftime(
            time_buffer, sizeof(time_buffer), "%Y-%m-%dT%H:%M:%SZ", tm_started);
        jvResult[jss::start_time] = time_buffer;

        // Add job type
        jvResult[jss::job_type] =
            (catalogueRunStatus.jobType == CatalogueJobType::CREATE)
            ? "catalogue_create"
            : "catalogue_load";

        // Add filename
        jvResult[jss::file] = catalogueRunStatus.filename;

        // Add compression level if applicable
        if (catalogueRunStatus.compressionLevel > 0)
        {
            jvResult[jss::compression_level] =
                catalogueRunStatus.compressionLevel;
        }

        // Add hash if available
        if (!catalogueRunStatus.hash.empty())
        {
            jvResult[jss::hash] = catalogueRunStatus.hash;
        }

        // Add filesize if available
        if (catalogueRunStatus.filesize > 0)
        {
            jvResult[jss::file_size_human] =
                formatBytesIEC(catalogueRunStatus.filesize);
            jvResult[jss::file_size] =
                std::to_string(catalogueRunStatus.filesize);
        }

        // Add estimated filesize ("unknown" if not available)
        jvResult[jss::file_size_estimated_human] =
            catalogueRunStatus.fileSizeEstimated;

        if (includeErrorInfo)
        {
            jvResult[jss::error] = "busy";
            jvResult[jss::error_message] =
                "Another catalogue operation is in progress";
        }
    }
    else
    {
        jvResult[jss::job_status] = "no_job_running";
    }

    return jvResult;
}

Json::Value
doCatalogueStatus(RPC::JsonContext& context)
{
    // Use a shared lock (read lock) to check status without blocking other
    // readers
    std::shared_lock<std::shared_mutex> lock(catalogueStatusMutex);
    return generateStatusJson();
}

Json::Value
doCatalogueCreate(RPC::JsonContext& context)
{
    // Try to acquire write lock to check if an operation is running
    {
        std::unique_lock<std::shared_mutex> writeLock(
            catalogueStatusMutex, std::try_to_lock);
        if (!writeLock.owns_lock())
        {
            // Couldn't get the lock, so another thread is accessing the status
            // Try a shared lock to get the status
            std::shared_lock<std::shared_mutex> readLock(catalogueStatusMutex);
            return generateStatusJson(true);
        }

        // We have the write lock, check if an operation is already running
        if (catalogueRunStatus.isRunning)
        {
            return generateStatusJson(true);
        }

        // No operation running, set up our operation
        catalogueRunStatus.isRunning = true;
    }
    // Write lock is released here, allowing status checks while operation runs

    // Ensure we reset the running flag when we're done
    struct OpCleanup
    {
        ~OpCleanup()
        {
            std::unique_lock<std::shared_mutex> writeLock(catalogueStatusMutex);
            catalogueRunStatus.isRunning = false;
        }
    } opCleanup;

    if (!context.params.isMember(jss::min_ledger) ||
        !context.params.isMember(jss::max_ledger))
        return rpcError(
            rpcINVALID_PARAMS, "expected min_ledger and max_ledger");

    std::string filepath;
    struct stat st;
    uint64_t file_size = 0;

    if (!context.params.isMember(jss::output_file) ||
        (filepath = context.params[jss::output_file].asString()).empty() ||
        filepath.front() != '/')
        return rpcError(
            rpcINVALID_PARAMS,
            "expected output_file: <absolute writeable filepath>");

    uint8_t compressionLevel = 0;  // Default: no compression

    if (context.params.isMember(jss::compression_level))
    {
        if (context.params[jss::compression_level].isInt() ||
            context.params[jss::compression_level].isUInt())
        {
            // Handle numeric value between 0 and 9
            compressionLevel = context.params[jss::compression_level].asUInt();
            if (compressionLevel > 9)
                compressionLevel = 9;
        }
        else if (context.params[jss::compression_level].isBool())
        {
            // Handle boolean: true means 6, false means 0
            compressionLevel =
                context.params[jss::compression_level].asBool() ? 6 : 0;
        }
    }

    // Check output file isn't already populated and can be written to
    {
        struct stat st;
        if (stat(filepath.c_str(), &st) == 0)
        {  // file exists
            if (st.st_size > 0)
                return rpcError(
                    rpcINVALID_PARAMS,
                    "output_file already exists and is non-empty");
        }
        else if (errno != ENOENT)
            return rpcError(
                rpcINTERNAL,
                "cannot stat output_file: " + std::string(strerror(errno)));

        std::ofstream testWrite(filepath.c_str(), std::ios::out);
        if (testWrite.fail())
            return rpcError(
                rpcINTERNAL,
                "output_file location is not writeable: " +
                    std::string(strerror(errno)));
        testWrite.close();
    }

    std::ofstream outfile(filepath.c_str(), std::ios::out | std::ios::binary);
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to open output_file: " + std::string(strerror(errno)));

    uint32_t min_ledger = context.params[jss::min_ledger].asUInt();
    uint32_t max_ledger = context.params[jss::max_ledger].asUInt();

    if (min_ledger > max_ledger)
        return rpcError(rpcINVALID_PARAMS, "min_ledger must be <= max_ledger");

    // Initialize status tracking
    {
        std::unique_lock<std::shared_mutex> writeLock(catalogueStatusMutex);
        catalogueRunStatus.isRunning = true;
        catalogueRunStatus.started = std::chrono::system_clock::now();
        catalogueRunStatus.minLedger = min_ledger;
        catalogueRunStatus.maxLedger = max_ledger;
        catalogueRunStatus.ledgerUpto =
            0;  // Initialize to 0 to indicate no progress yet
        catalogueRunStatus.jobType = CatalogueJobType::CREATE;
        catalogueRunStatus.filename = filepath;
        catalogueRunStatus.compressionLevel = compressionLevel;
        catalogueRunStatus.hash.clear();  // No hash yet
    }

    // Create and write header with zero hash
    CATLHeader header;
    header.min_ledger = min_ledger;
    header.max_ledger = max_ledger;
    header.version =
        makeCatalogueVersionField(CATALOGUE_VERSION, compressionLevel);
    header.network_id = context.app.config().NETWORK_ID;
    // hash is already zero-initialized

    outfile.write(reinterpret_cast<const char*>(&header), sizeof(CATLHeader));
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to write header: " + std::string(strerror(errno)));

    auto compStream = std::make_unique<boost::iostreams::filtering_ostream>();
    if (compressionLevel > 0)
    {
        JLOG(context.j.info())
            << "Setting up compression with level " << (int)compressionLevel;

        boost::iostreams::zlib_params params((int)compressionLevel);
        params.window_bits = 15;
        params.noheader = false;
        compStream->push(boost::iostreams::zlib_compressor(params));
    }
    else
    {
        JLOG(context.j.info())
            << "No compression (level 0), using direct output";
    }

    ByteCounterFilter byteCounter;
    compStream->push(boost::ref(byteCounter));

    compStream->push(boost::ref(outfile));

    // Process ledgers with local processor implementation
    auto writeToFile = [&compStream, &context](const void* data, size_t size) {
        compStream->write(reinterpret_cast<const char*>(data), size);
        if (compStream->fail())
        {
            JLOG(context.j.error())
                << "Failed to write to output file: " << std::strerror(errno);
            return false;
        }
        return true;
    };

    CatalogueSizePredictor predictor(
        header.min_ledger, header.max_ledger, sizeof(CATLHeader));

    // Modified outputLedger to work with individual ledgers instead of a vector
    auto outputLedger =
        [&writeToFile, &context, &compStream, &predictor, &byteCounter](
            std::shared_ptr<Ledger const> ledger,
            std::optional<std::reference_wrapper<const SHAMap>> prevStateMap =
                std::nullopt) -> bool {
        try
        {
            byteCounter.resetCounter();
            auto const& info = ledger->info();

            uint64_t closeTime = info.closeTime.time_since_epoch().count();
            uint64_t parentCloseTime =
                info.parentCloseTime.time_since_epoch().count();
            uint32_t closeTimeResolution = info.closeTimeResolution.count();
            uint64_t drops = info.drops.drops();

            // Write ledger header information
            if (!writeToFile(&info.seq, sizeof(info.seq)) ||
                !writeToFile(info.hash.data(), 32) ||
                !writeToFile(info.txHash.data(), 32) ||
                !writeToFile(info.accountHash.data(), 32) ||
                !writeToFile(info.parentHash.data(), 32) ||
                !writeToFile(&drops, sizeof(drops)) ||
                !writeToFile(&info.closeFlags, sizeof(info.closeFlags)) ||
                !writeToFile(
                    &closeTimeResolution, sizeof(closeTimeResolution)) ||
                !writeToFile(&closeTime, sizeof(closeTime)) ||
                !writeToFile(&parentCloseTime, sizeof(parentCloseTime)))
            {
                return false;
            }

            size_t stateNodesWritten = RPC::serializeStateMapToStream(
                ledger->stateMap(), *compStream, prevStateMap);
            size_t txNodesWritten =
                RPC::serializeTxMapToStream(ledger->txMap(), *compStream);

            predictor.addLedger(info.seq, byteCounter.getBytesWritten());

            JLOG(context.j.info()) << "Ledger " << info.seq << ": Wrote "
                                   << stateNodesWritten << " state nodes, "
                                   << "and " << txNodesWritten << " tx nodes";

            return true;
        }
        catch (std::exception const& e)
        {
            JLOG(context.j.error()) << "Error processing ledger "
                                    << ledger->info().seq << ": " << e.what();
            return false;
        }
    };

    // Instead of loading all ledgers at once, process them in a sliding window
    // of two
    std::shared_ptr<Ledger const> prevLedger = nullptr;
    std::shared_ptr<Ledger const> currLedger = nullptr;
    uint32_t ledgers_written = 0;

    JLOG(context.j.info()) << "Starting to stream ledgers from " << min_ledger
                           << " to " << max_ledger;

    // Process the first ledger completely
    {
        UPDATE_CATALOGUE_STATUS(ledgerUpto, min_ledger);

        // Load the first ledger
        if (auto error = RPC::getLedger(currLedger, min_ledger, context))
            return rpcError(error.toErrorCode(), error.message());
        if (!currLedger)
            return rpcError(rpcLEDGER_MISSING);

        if (!outputLedger(currLedger))
            return rpcError(
                rpcINTERNAL, "Error occurred while processing first ledger");

        ledgers_written++;
        prevLedger = currLedger;
    }

    // Process remaining ledgers with diffs
    for (uint32_t ledger_seq = min_ledger + 1; ledger_seq <= max_ledger;
         ++ledger_seq)
    {
        if (context.app.isStopping())
            return {};

        // Update current ledger in status
        UPDATE_CATALOGUE_STATUS(ledgerUpto, ledger_seq);

        // Load the next ledger
        currLedger = nullptr;  // Release any previous current ledger
        if (auto error = RPC::getLedger(currLedger, ledger_seq, context))
            return rpcError(error.toErrorCode(), error.message());
        if (!currLedger)
            return rpcError(rpcLEDGER_MISSING);

        // Process with diff against previous ledger
        if (!outputLedger(currLedger, prevLedger->stateMap()))
            return rpcError(
                rpcINTERNAL, "Error occurred while processing ledgers");

        UPDATE_CATALOGUE_STATUS(
            fileSizeEstimated, predictor.getEstimateHuman());

        ledgers_written++;

        // Cycle the ledgers: current becomes previous, we'll load a new current
        // next iteration
        prevLedger = currLedger;
    }

    // flush and finish
    compStream->flush();
    compStream->reset();
    outfile.flush();
    outfile.close();

    // Clear ledger references to release memory
    prevLedger = nullptr;
    currLedger = nullptr;

    // Get the file size and update it in the header
    if (stat(filepath.c_str(), &st) != 0)
    {
        JLOG(context.j.warn())
            << "Could not get file size: " << std::strerror(errno);
        return rpcError(
            rpcINTERNAL, "failed to get file size for header update");
    }

    file_size = st.st_size;

    // Update header with filesize
    JLOG(context.j.info()) << "Updating file size in header: "
                           << std::to_string(file_size) << " bytes";

    header.filesize = file_size;
    std::fstream updateFileSizeFile(
        filepath.c_str(), std::ios::in | std::ios::out | std::ios::binary);
    if (updateFileSizeFile.fail())
        return rpcError(
            rpcINTERNAL,
            "cannot open file for updating filesize: " +
                std::string(strerror(errno)));

    updateFileSizeFile.seekp(0, std::ios::beg);
    updateFileSizeFile.write(
        reinterpret_cast<const char*>(&header), sizeof(CATLHeader));
    updateFileSizeFile.close();

    // Now compute the hash over the entire file
    JLOG(context.j.info()) << "Computing catalogue hash...";

    std::ifstream hashFile(filepath.c_str(), std::ios::in | std::ios::binary);
    if (hashFile.fail())
        return rpcError(
            rpcINTERNAL,
            "cannot open file for hashing: " + std::string(strerror(errno)));

    // Initialize hasher
    sha512_hasher hasher;

    // Create a buffer for reading
    std::vector<char> buffer(64 * 1024);  // 64K buffer

    // Read and process the header portion
    hashFile.read(buffer.data(), sizeof(CATLHeader));
    if (hashFile.gcount() != sizeof(CATLHeader))
        return rpcError(rpcINTERNAL, "failed to read header for hashing");

    // Zero out the hash portion in the buffer for hash calculation
    std::fill(
        buffer.data() + offsetof(CATLHeader, hash),
        buffer.data() + offsetof(CATLHeader, hash) + sizeof(header.hash),
        0);

    // Add the modified header to the hash
    hasher(buffer.data(), sizeof(CATLHeader));

    // Read and hash the rest of the file
    while (hashFile)
    {
        hashFile.read(buffer.data(), buffer.size());
        std::streamsize bytes_read = hashFile.gcount();
        if (bytes_read > 0)
            hasher(buffer.data(), bytes_read);
    }
    hashFile.close();

    // Get the hash result
    auto hash_result = static_cast<sha512_hasher::result_type>(hasher);

    // Update the hash in the file
    std::fstream updateFile(
        filepath.c_str(), std::ios::in | std::ios::out | std::ios::binary);
    if (updateFile.fail())
        return rpcError(
            rpcINTERNAL,
            "cannot open file for updating hash: " +
                std::string(strerror(errno)));

    updateFile.seekp(offsetof(CATLHeader, hash), std::ios::beg);
    updateFile.write(
        reinterpret_cast<const char*>(hash_result.data()), hash_result.size());
    updateFile.close();

    // Convert hash to hex string
    std::string hash_hex = toHexString(hash_result.data(), hash_result.size());

    // Update status with hash and filesize
    UPDATE_CATALOGUE_STATUS(hash, hash_hex);
    UPDATE_CATALOGUE_STATUS(filesize, file_size);

    Json::Value jvResult;
    jvResult[jss::min_ledger] = min_ledger;
    jvResult[jss::max_ledger] = max_ledger;
    jvResult[jss::output_file] = filepath;
    jvResult[jss::file_size_human] = formatBytesIEC(file_size);
    jvResult[jss::file_size] = std::to_string(file_size);
    jvResult[jss::ledgers_written] = static_cast<Json::UInt>(ledgers_written);
    jvResult[jss::status] = jss::success;
    jvResult[jss::compression_level] = compressionLevel;
    jvResult[jss::hash] = hash_hex;

    return jvResult;
}

namespace {

// ---- catalogue_load tuning --------------------------------------------------

// state.db is updated at checkpoints rather than per ledger (it is opened with
// synchronous=FULL, so each update costs several fsyncs). In between, progress
// is visible in memory (complete_ledgers, pinned set); a crash loses at most
// the ledgers since the last checkpoint, and re-running catalogue_load on the
// same file re-covers them idempotently.
constexpr std::uint32_t loadCheckpointLedgers = 1024;
constexpr std::chrono::seconds loadCheckpointInterval{10};

// A pinned backend that cannot sync on demand (NuDB commits on its own thread
// about once a second) gets this long to commit the tail before the final
// state.db update. SHAMapStoreImp::loadPinnedRanges trims anything that still
// did not make it, so this only narrows the window.
constexpr std::chrono::seconds nonDurableCommitGrace{3};

// A ledger's nodes are written only after its hash verifies. Maps too large
// to buffer (the full base ledger) spill in chunks of this size instead.
constexpr std::size_t pinnedBatchMaxObjects = 65536;
constexpr std::size_t pinnedBatchMaxBytes = 64 * 1024 * 1024;

// Buffer size for reads from the file and through the decompressor.
constexpr std::streamsize loadReadBufferSize = 1 << 20;

// ---- helpers ----------------------------------------------------------------

struct CatalogueHashState
{
    sha512_hasher hasher;
    std::uint64_t bytes = 0;  // raw file bytes hashed after the header
};

// Sits directly above the file in the input chain, beneath any decompressor,
// so the whole-file SHA-512 is computed in the same pass that loads ledgers.
class CatalogueHashTee : public boost::iostreams::multichar_input_filter
{
    CatalogueHashState* state_;

public:
    explicit CatalogueHashTee(CatalogueHashState& state) : state_(&state)
    {
    }

    template <typename Source>
    std::streamsize
    read(Source& src, char* s, std::streamsize n)
    {
        std::streamsize const r = boost::iostreams::read(src, s, n);
        if (r > 0)
        {
            state_->hasher(s, static_cast<std::size_t>(r));
            state_->bytes += static_cast<std::uint64_t>(r);
        }
        return r;
    }
};

// Collects the nodes a ledger's maps flush (via SHAMap::flushDirty(t, sink),
// which bypasses the TreeNodeCache) and writes them to the pinned backend as
// one batch.
class PinnedBatchWriter
{
public:
    explicit PinnedBatchWriter(NodeStore::DatabasePinnedImp& db)
        : db_(db)
        , sink_([this](NodeObjectType type, Blob&& data, uint256 const& hash) {
            bytes_ += data.size();
            batch_.emplace_back(
                NodeObject::createObject(type, std::move(data), hash));
            if (batch_.size() >= pinnedBatchMaxObjects ||
                bytes_ >= pinnedBatchMaxBytes)
                write();
        })
    {
        batch_.reserve(4096);
    }

    PinnedBatchWriter(PinnedBatchWriter const&) = delete;
    PinnedBatchWriter&
    operator=(PinnedBatchWriter const&) = delete;

    SHAMap::FlushSink const&
    sink() const
    {
        return sink_;
    }

    void
    write()
    {
        if (batch_.empty())
            return;
        db_.storePinnedBatch(batch_);
        batch_.clear();
        bytes_ = 0;
    }

    void
    discard()
    {
        batch_.clear();
        bytes_ = 0;
    }

private:
    NodeStore::DatabasePinnedImp& db_;
    NodeStore::Batch batch_;
    std::size_t bytes_ = 0;
    SHAMap::FlushSink sink_;
};

// Makes the pinned set durable in state.db, but only as far as the pinned
// backend has made the corresponding nodes durable. A backend that can sync
// on demand (RocksDB) is synced and the current set recorded; one that can't
// (NuDB) records the previous checkpoint's set, which is at least one
// interval old.
class PinnedRangeCheckpointer
{
    using clock = std::chrono::steady_clock;

public:
    PinnedRangeCheckpointer(Application& app, NodeStore::DatabasePinnedImp& db)
        : app_(app), db_(db), last_(clock::now())
    {
    }

    void
    onLedgerSaved()
    {
        ++saved_;
        if (++sinceLast_ < loadCheckpointLedgers &&
            clock::now() - last_ < loadCheckpointInterval)
            return;

        auto current = app_.getLedgerMaster().getPinnedLedgersRangeSet();
        if (db_.syncPinned())
            persist(current);
        else
        {
            if (previous_)
                persist(*previous_);
            previous_ = std::move(current);
        }
        sinceLast_ = 0;
        last_ = clock::now();
    }

    /** Record everything pinned so far. On shutdown the backend is closed
        right after, which commits whatever is still pending. */
    void
    finish(bool stopping)
    {
        if (saved_ == 0)
            return;
        if (!db_.syncPinned() && !stopping)
            std::this_thread::sleep_for(nonDurableCommitGrace);
        persist(app_.getLedgerMaster().getPinnedLedgersRangeSet());
    }

private:
    void
    persist(RangeSet<std::uint32_t> const& ranges)
    {
        app_.getSHAMapStore().setPinnedRanges(ranges);
    }

    Application& app_;
    NodeStore::DatabasePinnedImp& db_;
    std::optional<RangeSet<std::uint32_t>> previous_;
    std::uint32_t saved_ = 0;
    std::uint32_t sinceLast_ = 0;
    clock::time_point last_;
};

std::optional<std::uint32_t>
asLedgerSeq(Json::Value const& v)
{
    if (v.isUInt())
        return v.asUInt();
    if (v.isInt() && v.asInt() >= 0)
        return static_cast<std::uint32_t>(v.asInt());
    return std::nullopt;
}

}  // namespace

Json::Value
doCatalogueLoad(RPC::JsonContext& context)
{
    // Try to acquire write lock to check if an operation is running
    {
        std::unique_lock<std::shared_mutex> writeLock(
            catalogueStatusMutex, std::try_to_lock);
        if (!writeLock.owns_lock())
        {
            // Couldn't get the lock, so another thread is accessing the status
            // Try a shared lock to get the status
            std::shared_lock<std::shared_mutex> readLock(catalogueStatusMutex);
            return generateStatusJson(true);
        }

        // We have the write lock, check if an operation is already running
        if (catalogueRunStatus.isRunning)
        {
            return generateStatusJson(true);
        }

        // No operation running, set up our operation
        catalogueRunStatus.isRunning = true;
    }
    // Write lock is released here, allowing status checks while operation runs

    // Ensure we reset the running flag when we're done
    struct OpCleanup
    {
        ~OpCleanup()
        {
            std::unique_lock<std::shared_mutex> writeLock(catalogueStatusMutex);
            catalogueRunStatus.isRunning = false;
        }
    } opCleanup;

    auto& app = context.app;
    auto& ledgerMaster = app.getLedgerMaster();
    auto const j = context.j;

    // Reject if DatabasePinned is not configured. Without it, loaded
    // data lands in rotating storage and will be rotated away.
    {
        auto const& nscfg = app.config().section(ConfigSection::nodeDatabase());
        if (!nscfg.exists("pinned_type"))
        {
            return rpcError(
                rpcINVALID_PARAMS,
                "catalogue_load requires [node_db] pinned_type to be "
                "configured. Without it, loaded data will be lost on "
                "the next database rotation.");
        }
    }

    auto* const pinnedDb =
        dynamic_cast<NodeStore::DatabasePinnedImp*>(&app.getNodeStore());
    if (!pinnedDb)
        return rpcError(
            rpcINTERNAL,
            "catalogue_load requires the pinned node store, which is not "
            "active.");

    if (!context.params.isMember(jss::input_file))
        return rpcError(rpcINVALID_PARAMS, "expected input_file");

    // Check for ignore_hash parameter
    bool ignore_hash = false;
    if (context.params.isMember(jss::ignore_hash))
        ignore_hash = context.params[jss::ignore_hash].asBool();

    std::string filepath = context.params[jss::input_file].asString();
    if (filepath.empty() || filepath.front() != '/')
        return rpcError(
            rpcINVALID_PARAMS,
            "expected input_file: <absolute readable filepath>");

    JLOG(j.info()) << "Opening catalogue file: " << filepath;

    // Check file size before attempting to read
    struct stat st;
    if (stat(filepath.c_str(), &st) != 0)
        return rpcError(
            rpcINTERNAL,
            "cannot stat input_file: " + std::string(strerror(errno)));

    uint64_t file_size = st.st_size;

    // Minimal size check: at least a header must be present
    if (file_size < sizeof(CATLHeader))
        return rpcError(
            rpcINVALID_PARAMS,
            "input_file too small (only " + std::to_string(file_size) +
                " bytes), must be at least " +
                std::to_string(sizeof(CATLHeader)) + " bytes");

    JLOG(j.info()) << "Catalogue file size: " << file_size << " bytes";

    // Check if file exists and is readable
    std::ifstream infile(filepath.c_str(), std::ios::in | std::ios::binary);
    if (infile.fail())
        return rpcError(
            rpcINTERNAL,
            "cannot open input_file: " + std::string(strerror(errno)));

    JLOG(j.info()) << "Reading catalogue header...";

    // Read and validate header
    CATLHeader header;
    infile.read(reinterpret_cast<char*>(&header), sizeof(CATLHeader));
    if (infile.fail())
        return rpcError(rpcINTERNAL, "failed to read catalogue header");

    if (header.magic != CATL)
        return rpcError(rpcINVALID_PARAMS, "invalid catalogue file magic");

    // Save the hash from the header
    std::array<uint8_t, 64> stored_hash = header.hash;
    std::string hash_hex = toHexString(stored_hash.data(), stored_hash.size());

    // Extract version information
    uint8_t version = getCatalogueVersion(header.version);
    uint8_t compressionLevel = getCompressionLevel(header.version);

    if (header.min_ledger > header.max_ledger)
        return rpcError(
            rpcINVALID_PARAMS, "catalogue min_ledger must be <= max_ledger");

    // Initialize status tracking
    {
        std::unique_lock<std::shared_mutex> writeLock(catalogueStatusMutex);
        catalogueRunStatus.isRunning = true;
        catalogueRunStatus.started = std::chrono::system_clock::now();
        catalogueRunStatus.minLedger = header.min_ledger;
        catalogueRunStatus.maxLedger = header.max_ledger;
        catalogueRunStatus.ledgerUpto =
            0;  // Initialize to 0 to indicate no progress yet
        catalogueRunStatus.jobType = CatalogueJobType::LOAD;
        catalogueRunStatus.filename = filepath;
        catalogueRunStatus.compressionLevel = compressionLevel;
        catalogueRunStatus.hash = hash_hex;
        catalogueRunStatus.filesize = header.filesize;
    }

    JLOG(j.info()) << "Catalogue version: " << (int)version;
    JLOG(j.info()) << "Compression level: " << (int)compressionLevel;
    JLOG(j.info()) << "Catalogue hash: " << hash_hex;

    // Check version compatibility
    if (version > 1)  // Only checking base version number
        return rpcError(
            rpcINVALID_PARAMS,
            "unsupported catalogue version: " + std::to_string(version));

    if (header.network_id != app.config().NETWORK_ID)
        return rpcError(
            rpcINVALID_PARAMS,
            "catalogue network ID mismatch: " +
                std::to_string(header.network_id));

    // Check if actual filesize matches the one in the header
    if (file_size != header.filesize)
    {
        JLOG(j.error()) << "Catalogue file size mismatch. Header indicates "
                        << header.filesize << " bytes, but actual file size is "
                        << file_size << " bytes";
        return rpcError(
            rpcINVALID_PARAMS,
            "catalogue file size mismatch: expected " +
                std::to_string(header.filesize) + " bytes, got " +
                std::to_string(file_size) + " bytes");
    }

    JLOG(j.info()) << "Catalogue file size verified: " << file_size << " bytes";

    // ---- Authenticity anchors ----------------------------------------------
    //
    // Each ledger's hash is recomputed from its contents and must equal the
    // hash in the file, and each ledger's parentHash must equal the previous
    // ledger's computed hash, so the file is internally consistent. On a
    // networked server it is also tied to history this server already trusts:
    //  - the base ledger must chain to the local ledger min-1, if we have it;
    //  - the last ledger must be the parent of the local ledger max+1, if any;
    //  - any ledger the validated ledger's skip lists know (every 256th, and
    //    the last 256) must match. Through the parent chain, one match
    //    authenticates every earlier ledger in the file.
    // Standalone servers have unrelated local history, so these are skipped.
    std::shared_ptr<Ledger const> anchor;
    std::optional<uint256> localParentOfMin;
    std::optional<uint256> localHashOfMax;
    if (!app.config().standalone())
    {
        auto& rdb = app.getRelationalDatabase();
        anchor = ledgerMaster.getValidatedLedger();
        if (header.min_ledger > 1)
        {
            if (auto const h = rdb.getHashByIndex(header.min_ledger - 1);
                h.isNonZero())
                localParentOfMin = h;
        }
        if (header.max_ledger < std::numeric_limits<std::uint32_t>::max())
        {
            if (auto const next =
                    rdb.getLedgerInfoByIndex(header.max_ledger + 1))
                localHashOfMax = next->parentHash;
        }
    }
    std::optional<std::uint32_t> anchoredThrough;

    // ---- Single pass: hash, decompress, parse, verify, store ---------------
    //
    // The whole-file SHA-512 is computed by a tee beneath the decompressor
    // instead of a separate pre-pass over the file. Per-ledger hash checks
    // catch corruption at the ledger it hits; the file hash is settled when
    // loading ends, successfully or not, so a file failing its checksum is
    // reported as such and keeps nothing this run pinned.
    bool const verifyHash = !ignore_hash && file_size > sizeof(CATLHeader);
    CatalogueHashState hashState;
    if (verifyHash)
    {
        CATLHeader hashHeader = header;
        std::fill(hashHeader.hash.begin(), hashHeader.hash.end(), 0);
        hashState.hasher(&hashHeader, sizeof(CATLHeader));
    }

    auto decompStream = std::make_unique<boost::iostreams::filtering_istream>();
    if (compressionLevel > 0)
    {
        JLOG(j.info()) << "Setting up decompression with level "
                       << (int)compressionLevel;
        boost::iostreams::zlib_params params((int)compressionLevel);
        params.window_bits = 15;
        params.noheader = false;
        decompStream->push(
            boost::iostreams::zlib_decompressor(params, loadReadBufferSize),
            loadReadBufferSize);
    }
    else
    {
        JLOG(j.info())
            << "No decompression needed (level 0), using direct input";
    }
    if (verifyHash)
        decompStream->push(CatalogueHashTee(hashState), loadReadBufferSize);
    decompStream->push(boost::ref(infile), loadReadBufferSize);

    // Hash the raw bytes the decoder did not consume (normally none, all of
    // them after a mid-file failure) and compare with the header.
    auto fileHashMatches = [&]() -> bool {
        if (!verifyHash)
            return true;

        std::ifstream rest(filepath.c_str(), std::ios::in | std::ios::binary);
        if (rest.fail())
            return false;
        rest.seekg(
            static_cast<std::streamoff>(sizeof(CATLHeader) + hashState.bytes),
            std::ios::beg);
        std::vector<char> buffer(loadReadBufferSize);
        while (rest)
        {
            rest.read(buffer.data(), buffer.size());
            auto const n = rest.gcount();
            if (n > 0)
                hashState.hasher(buffer.data(), static_cast<std::size_t>(n));
        }

        auto const computed =
            static_cast<sha512_hasher::result_type>(hashState.hasher);
        if (std::equal(computed.begin(), computed.end(), stored_hash.begin()))
        {
            JLOG(j.info()) << "Catalogue hash verified successfully";
            return true;
        }
        JLOG(j.error()) << "Catalogue hash verification failed. Expected: "
                        << hash_hex << ", Computed: "
                        << toHexString(computed.data(), computed.size());
        return false;
    };

    auto const pinnedBefore = ledgerMaster.getPinnedLedgersRangeSet();
    auto const completeBefore = ledgerMaster.getCompleteLedgersRangeSet();

    PinnedBatchWriter batch(*pinnedDb);
    PinnedRangeCheckpointer checkpointer(app, *pinnedDb);

    uint32_t ledgersLoaded = 0;
    std::shared_ptr<Ledger> prevLedger;
    uint32_t expected_seq = header.min_ledger;

    // Pinned in memory by storeLedger but not yet saved; unpinned on failure.
    std::optional<std::uint32_t> pendingPin;

    // Undo this run's pins (used when the file fails its checksum).
    auto rollback = [&]() {
        if (ledgersLoaded == 0)
            return;
        RangeSet<std::uint32_t> loaded;
        loaded.insert(
            range(header.min_ledger, header.min_ledger + ledgersLoaded - 1));
        auto const unpin = loaded - pinnedBefore;
        if (unpin.empty())
            return;
        ledgerMaster.unpinLedgers(unpin, unpin - completeBefore);
        app.getSHAMapStore().setPinnedRanges(
            ledgerMaster.getPinnedLedgersRangeSet());
        JLOG(j.warn()) << "Catalogue failed its checksum; unpinned "
                       << to_string(unpin) << " loaded by this run";
    };

    // Every exit once loading has started goes through here.
    auto conclude = [&](Json::Value result) -> Json::Value {
        if (pendingPin)
        {
            ledgerMaster.unpinLedger(*pendingPin);
            pendingPin.reset();
        }
        if (app.isStopping())
        {
            checkpointer.finish(true);
            return result;
        }
        if (!fileHashMatches())
        {
            rollback();
            return rpcError(
                rpcINVALID_PARAMS, "catalogue hash verification failed");
        }
        checkpointer.finish(false);
        return result;
    };

    try
    {
        // Process each ledger sequentially
        while (!decompStream->eof() && expected_seq <= header.max_ledger)
        {
            if (app.isStopping())
                return conclude({});

            // Update current ledger
            UPDATE_CATALOGUE_STATUS(ledgerUpto, expected_seq);

            LedgerInfo info;
            uint64_t closeTime = -1;
            uint64_t parentCloseTime = -1;
            uint32_t closeTimeResolution = -1;
            uint64_t drops = -1;

            if (!decompStream->read(
                    reinterpret_cast<char*>(&info.seq), sizeof(info.seq)) ||
                !decompStream->read(
                    reinterpret_cast<char*>(info.hash.data()), 32) ||
                !decompStream->read(
                    reinterpret_cast<char*>(info.txHash.data()), 32) ||
                !decompStream->read(
                    reinterpret_cast<char*>(info.accountHash.data()), 32) ||
                !decompStream->read(
                    reinterpret_cast<char*>(info.parentHash.data()), 32) ||
                !decompStream->read(
                    reinterpret_cast<char*>(&drops), sizeof(drops)) ||
                !decompStream->read(
                    reinterpret_cast<char*>(&info.closeFlags),
                    sizeof(info.closeFlags)) ||
                !decompStream->read(
                    reinterpret_cast<char*>(&closeTimeResolution),
                    sizeof(closeTimeResolution)) ||
                !decompStream->read(
                    reinterpret_cast<char*>(&closeTime), sizeof(closeTime)) ||
                !decompStream->read(
                    reinterpret_cast<char*>(&parentCloseTime),
                    sizeof(parentCloseTime)))
            {
                JLOG(j.warn())
                    << "Catalogue load expected but could not "
                    << "read the next ledger header at seq=" << expected_seq
                    << ". "
                    << "Ledgers prior to this in the file (if any) were "
                       "loaded.";
                return conclude(
                    rpcError(rpcINTERNAL, "Unexpected end of catalogue file."));
            }

            info.closeTime = time_point{duration{closeTime}};
            info.parentCloseTime = time_point{duration{parentCloseTime}};
            info.closeTimeResolution = duration{closeTimeResolution};
            info.drops = drops;

            JLOG(j.debug()) << "Found ledger " << info.seq << "...";

            if (info.seq != expected_seq)
            {
                JLOG(j.error()) << "Expected ledger " << expected_seq
                                << ", found " << info.seq << ", bailing";
                return conclude(rpcError(
                    rpcINTERNAL,
                    "Unexpected ledger out of sequence in catalogue file"));
            }
            ++expected_seq;

            // Chain linkage: the file's ledgers must form one chain, and the
            // base ledger must extend local history where we have it.
            if (prevLedger)
            {
                if (info.parentHash != prevLedger->info().hash)
                {
                    JLOG(j.error())
                        << "Catalogue ledger " << info.seq << " has parent "
                        << info.parentHash << " but ledger " << (info.seq - 1)
                        << " is " << prevLedger->info().hash;
                    return conclude(rpcError(
                        rpcINTERNAL,
                        "Catalogue ledger " + std::to_string(info.seq) +
                            " does not chain to its predecessor."));
                }
            }
            else if (localParentOfMin && info.parentHash != *localParentOfMin)
            {
                return conclude(rpcError(
                    rpcINVALID_PARAMS,
                    "Catalogue ledger " + std::to_string(info.seq) +
                        " does not chain to local ledger " +
                        std::to_string(info.seq - 1) + "."));
            }

            // Create a ledger object
            std::shared_ptr<Ledger> ledger;

            if (info.seq == header.min_ledger)
            {
                // Base ledger - create a fresh one
                ledger = std::make_shared<Ledger>(
                    info.seq,
                    info.closeTime,
                    app.config(),
                    app.getNodeFamily());

                ledger->setLedgerInfo(info);

                // Deserialize the complete state map from leaf nodes
                if (!RPC::deserializeStateMapFromStream(
                        ledger->stateMap(), *decompStream, batch.sink(), j))
                {
                    JLOG(j.error())
                        << "Failed to deserialize base ledger state";
                    batch.discard();
                    return conclude(rpcError(
                        rpcINTERNAL, "Failed to load base ledger state"));
                }
            }
            else
            {
                // Delta ledger - start with a copy of the previous ledger
                if (!prevLedger)
                {
                    JLOG(j.error()) << "Missing previous ledger for delta";
                    return conclude(
                        rpcError(rpcINTERNAL, "Missing previous ledger"));
                }

                auto snapshot = prevLedger->stateMap().snapShot(true);

                ledger = std::make_shared<Ledger>(
                    info, app.config(), app.getNodeFamily(), *snapshot);

                // Apply delta (only leaf-node changes)
                if (!RPC::deserializeStateMapFromStream(
                        ledger->stateMap(), *decompStream, batch.sink(), j))
                {
                    JLOG(j.error())
                        << "Failed to apply delta to ledger " << info.seq;
                    batch.discard();
                    return conclude(
                        rpcError(rpcINTERNAL, "Failed to apply ledger delta"));
                }
            }

            // pull in the tx map
            if (!RPC::deserializeTxMapFromStream(
                    ledger->txMap(), *decompStream, batch.sink(), j))
            {
                JLOG(j.error())
                    << "Failed to load transactions of ledger " << info.seq;
                batch.discard();
                return conclude(
                    rpcError(rpcINTERNAL, "Failed to apply ledger delta"));
            }

            ledger->setAccepted(
                info.closeTime,
                info.closeTimeResolution,
                info.closeFlags & sLCF_NoConsensusTime);

            ledger->setValidated();
            ledger->setCloseFlags(info.closeFlags);
            ledger->setImmutable(true);

            // we can double check the computed hashes now, since setImmutable
            // recomputes the hashes
            if (ledger->info().hash != info.hash)
            {
                JLOG(j.error())
                    << "Ledger seq=" << info.seq
                    << " was loaded from catalogue, but computed hash does "
                       "not match. "
                    << "This ledger was not saved, and ledger loading from "
                       "this catalogue file ended here.";
                batch.discard();
                return conclude(rpcError(
                    rpcINTERNAL,
                    "Catalogue file contains a corrupted ledger."));
            }

            if (localHashOfMax && info.seq == header.max_ledger &&
                info.hash != *localHashOfMax)
            {
                batch.discard();
                return conclude(rpcError(
                    rpcINVALID_PARAMS,
                    "Catalogue ledger " + std::to_string(info.seq) +
                        " is not the parent of local ledger " +
                        std::to_string(info.seq + 1) + "."));
            }

            if (anchor && info.seq <= anchor->info().seq &&
                ((info.seq & 0xff) == 0 ||
                 anchor->info().seq - info.seq <= 256))
            {
                if (auto const h = hashOfSeq(*anchor, info.seq, j))
                {
                    if (*h != info.hash)
                    {
                        batch.discard();
                        return conclude(rpcError(
                            rpcINVALID_PARAMS,
                            "Catalogue ledger " + std::to_string(info.seq) +
                                " does not match validated network "
                                "history."));
                    }
                    anchoredThrough = info.seq;
                }
            }

            // Verified: now its nodes go to the pinned store, before its
            // header (written by the save below), so that a present header
            // implies the ledger's nodes are present.
            batch.write();

            // IMPORTANT: Mark as pinned BEFORE saving to database.
            // This ensures isPinned() returns true when saveValidatedLedger
            // checks, which: (a) routes to persistent backend via
            // pinnedLEDGER type, and (b) skips AcceptedLedgerCache and the
            // per-transaction JSON.
            ledgerMaster.storeLedger(ledger, true);
            pendingPin = info.seq;

            // Save in database - wait for completion to avoid memory bloat.
            // Uses pendSaveValidated to respect job queue tuning on live
            // servers (jtPUBOLDLEDGER), runs synchronously in standalone.
            {
                auto savePromise = std::make_shared<std::promise<bool>>();
                auto saveFuture = savePromise->get_future();

                bool queued = pendSaveValidated(
                    app,
                    ledger,
                    app.config().standalone(),
                    false,
                    [savePromise](bool success) {
                        savePromise->set_value(success);
                    });

                if (!queued)
                    return conclude(
                        rpcError(rpcINTERNAL, "Failed to save ledger"));

                // Wait for the async save to complete. Note: if the save
                // job throws, the JobQueue has no exception handling — the
                // process will std::terminate before we ever see a
                // broken_promise here. The catch is defensive in case the
                // job queue gains exception handling in the future.
                bool saved = false;
                try
                {
                    saved = saveFuture.get();
                }
                catch (std::future_error const& e)
                {
                    JLOG(j.error())
                        << "Save job for ledger " << info.seq
                        << " failed with exception (promise broken): "
                        << e.what();
                    return conclude(rpcError(
                        rpcINTERNAL,
                        "Save job crashed for ledger " +
                            std::to_string(info.seq)));
                }

                if (!saved)
                {
                    JLOG(j.error())
                        << "Failed to save ledger " << info.seq << " to SQLite";
                    return conclude(rpcError(
                        rpcINTERNAL,
                        "Failed to save ledger " + std::to_string(info.seq) +
                            " to SQLite database"));
                }
            }
            pendingPin.reset();

            ledgerMaster.setLedgerRangePresent(
                header.min_ledger, info.seq, true);

            // Persist pinned ranges to state.db at checkpoints (see
            // PinnedRangeCheckpointer). Ledgers saved since the last
            // checkpoint are on disk but not yet recorded; if the process
            // dies before the next one, re-running catalogue_load on the
            // same file records them.
            checkpointer.onLedgerSaved();

            // Store the ledger
            prevLedger = ledger;
            ledgersLoaded++;
        }
    }
    catch (std::exception const& e)
    {
        JLOG(j.error()) << "Exception during catalogue load: " << e.what();
        batch.discard();
        return conclude(rpcError(
            rpcINTERNAL,
            std::string("Exception during catalogue load: ") + e.what()));
    }

    // Settle the whole-file hash and make the pinned ranges durable before
    // handing the last ledger to the rest of the server.
    auto result = conclude(Json::Value{Json::objectValue});
    if (result.isMember(jss::error) || app.isStopping())
        return result;

    if (prevLedger && prevLedger->info().seq == header.max_ledger &&
        ledgerMaster.getClosedLedger()->info().seq < header.max_ledger)
    {
        // Set as current ledger if this is the latest
        ledgerMaster.switchLCL(prevLedger);
    }

    decompStream->reset();
    infile.close();

    JLOG(j.info()) << "Catalogue load complete! Loaded " << ledgersLoaded
                   << " ledgers from file size " << file_size << " bytes";

    Json::Value jvResult;
    jvResult[jss::ledger_min] = header.min_ledger;
    jvResult[jss::ledger_max] = header.max_ledger;
    jvResult[jss::ledger_count] =
        static_cast<Json::UInt>(header.max_ledger - header.min_ledger + 1);
    jvResult[jss::ledgers_loaded] = static_cast<Json::UInt>(ledgersLoaded);
    jvResult[jss::file_size_human] = formatBytesIEC(file_size);
    jvResult[jss::file_size] = std::to_string(file_size);
    jvResult[jss::status] = jss::success;
    jvResult[jss::compression_level] = compressionLevel;
    jvResult[jss::hash] = hash_hex;
    jvResult[jss::ignore_hash] = ignore_hash;
    if (anchoredThrough)
        jvResult[jss::anchored_through] = *anchoredThrough;

    return jvResult;
}

// catalogue_unpin <ledger_index_min> <ledger_index_max>
//
// Stops protecting [ledger_index_min, ledger_index_max] (inclusive) from
// online_delete. Ledgers in the range that are not pinned are ignored.
//
// Unpinned ledgers stay readable until the next online_delete rotation, which
// releases them like any other history older than the rotation boundary (SQL
// rows and complete_ledgers; see SHAMapStoreImp::clearPrior). Their nodes stay
// in the pinned store: nodes are content-addressed and shared between ledgers
// (and NuDB cannot delete). Once nothing is pinned and one rotation has run,
// the hot store holds everything live state needs (DatabasePinnedImp copies
// persistent-only nodes forward during rotation), so pinned_path can then be
// discarded to reclaim its space.
Json::Value
doCatalogueUnpin(RPC::JsonContext& context)
{
    // Held throughout: catalogue_load extends and checkpoints the pinned set,
    // so an unpin must not interleave with it.
    std::unique_lock<std::shared_mutex> lock(
        catalogueStatusMutex, std::try_to_lock);
    if (!lock.owns_lock() || catalogueRunStatus.isRunning)
        return rpcError(
            rpcTOO_BUSY,
            "a catalogue operation is in progress; retry when it completes");

    auto& app = context.app;
    {
        auto const& nscfg = app.config().section(ConfigSection::nodeDatabase());
        if (!nscfg.exists("pinned_type"))
            return rpcError(
                rpcINVALID_PARAMS,
                "catalogue_unpin requires [node_db] pinned_type to be "
                "configured.");
    }

    auto const& params = context.params;
    if (!params.isMember(jss::ledger_index_min) ||
        !params.isMember(jss::ledger_index_max))
        return rpcError(
            rpcINVALID_PARAMS,
            "expected ledger_index_min and ledger_index_max");

    auto const minSeq = asLedgerSeq(params[jss::ledger_index_min]);
    auto const maxSeq = asLedgerSeq(params[jss::ledger_index_max]);
    if (!minSeq || !maxSeq)
        return rpcError(
            rpcINVALID_PARAMS,
            "ledger_index_min and ledger_index_max must be unsigned integers");
    if (*minSeq > *maxSeq)
        return rpcError(
            rpcINVALID_PARAMS, "ledger_index_min must be <= ledger_index_max");

    auto& ledgerMaster = app.getLedgerMaster();

    RangeSet<std::uint32_t> requested;
    requested.insert(range(*minSeq, *maxSeq));
    auto const unpinned = ledgerMaster.unpinLedgers(requested);
    auto const remaining = ledgerMaster.getPinnedLedgersRangeSet();

    if (!unpinned.empty())
    {
        app.getSHAMapStore().setPinnedRanges(remaining);
        JLOG(context.j.info()) << "Unpinned ledgers " << to_string(unpinned)
                               << "; still pinned: " << to_string(remaining);
    }

    Json::Value jvResult;
    jvResult[jss::unpinned] = to_string(unpinned);
    jvResult[jss::complete_ledgers_pinned] = to_string(remaining);
    jvResult[jss::status] = jss::success;
    return jvResult;
}

}  // namespace ripple
