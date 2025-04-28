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

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/LedgerToJson.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/tx/apply.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/Slice.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/LedgerFormats.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/GRPCHandlers.h>
#include <ripple/rpc/Role.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/impl/Tuning.h>
#include <ripple/shamap/SHAMapItem.h>

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>

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
    [[maybe_unused]] uint64_t headerSize_;

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
        , headerSize_(headerSize)
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

            size_t stateNodesWritten =
                ledger->stateMap().serializeToStream(*compStream, prevStateMap);
            size_t txNodesWritten =
                ledger->txMap().serializeToStream(*compStream);

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

    JLOG(context.j.info()) << "Opening catalogue file: " << filepath;

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

    JLOG(context.j.info()) << "Catalogue file size: " << file_size << " bytes";

    // Check if file exists and is readable
    std::ifstream infile(filepath.c_str(), std::ios::in | std::ios::binary);
    if (infile.fail())
        return rpcError(
            rpcINTERNAL,
            "cannot open input_file: " + std::string(strerror(errno)));

    JLOG(context.j.info()) << "Reading catalogue header...";

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

    JLOG(context.j.info()) << "Catalogue version: " << (int)version;
    JLOG(context.j.info()) << "Compression level: " << (int)compressionLevel;
    JLOG(context.j.info()) << "Catalogue hash: " << hash_hex;

    // Check version compatibility
    if (version > 1)  // Only checking base version number
        return rpcError(
            rpcINVALID_PARAMS,
            "unsupported catalogue version: " + std::to_string(version));

    if (header.network_id != context.app.config().NETWORK_ID)
        return rpcError(
            rpcINVALID_PARAMS,
            "catalogue network ID mismatch: " +
                std::to_string(header.network_id));

    // Check if actual filesize matches the one in the header
    if (file_size != header.filesize)
    {
        JLOG(context.j.error())
            << "Catalogue file size mismatch. Header indicates "
            << header.filesize << " bytes, but actual file size is "
            << file_size << " bytes";
        return rpcError(
            rpcINVALID_PARAMS,
            "catalogue file size mismatch: expected " +
                std::to_string(header.filesize) + " bytes, got " +
                std::to_string(file_size) + " bytes");
    }

    JLOG(context.j.info()) << "Catalogue file size verified: " << file_size
                           << " bytes";

    // Verify hash if not ignored
    if (!ignore_hash && file_size > sizeof(CATLHeader))
    {
        JLOG(context.j.info()) << "Verifying catalogue hash...";

        // Close and reopen file for hash verification
        infile.close();
        std::ifstream hashFile(
            filepath.c_str(), std::ios::in | std::ios::binary);
        if (hashFile.fail())
            return rpcError(
                rpcINTERNAL,
                "cannot reopen file for hash verification: " +
                    std::string(strerror(errno)));

        // Create a copy of the header with zeroed hash
        CATLHeader hashHeader = header;
        std::fill(hashHeader.hash.begin(), hashHeader.hash.end(), 0);

        // Initialize hasher
        sha512_hasher hasher;

        // Add the modified header to the hash
        hasher(&hashHeader, sizeof(CATLHeader));

        // Read and hash the rest of the file
        hashFile.seekg(sizeof(CATLHeader), std::ios::beg);
        std::vector<char> buffer(64 * 1024);  // 64K buffer
        while (hashFile)
        {
            if (context.app.isStopping())
                return {};

            hashFile.read(buffer.data(), buffer.size());
            std::streamsize bytes_read = hashFile.gcount();
            if (bytes_read > 0)
                hasher(buffer.data(), bytes_read);
        }
        hashFile.close();

        // Get the computed hash
        auto computed_hash = static_cast<sha512_hasher::result_type>(hasher);

        // Compare with stored hash
        if (!std::equal(
                computed_hash.begin(),
                computed_hash.end(),
                stored_hash.begin()))
        {
            std::string computed_hex =
                toHexString(computed_hash.data(), computed_hash.size());
            JLOG(context.j.error())
                << "Catalogue hash verification failed. Expected: " << hash_hex
                << ", Computed: " << computed_hex;
            return rpcError(
                rpcINVALID_PARAMS, "catalogue hash verification failed");
        }

        JLOG(context.j.info()) << "Catalogue hash verified successfully";

        // Reopen file for reading
        infile.open(filepath.c_str(), std::ios::in | std::ios::binary);
        if (infile.fail())
            return rpcError(
                rpcINTERNAL,
                "cannot reopen file after hash verification: " +
                    std::string(strerror(errno)));

        // Skip the header
        infile.seekg(sizeof(CATLHeader), std::ios::beg);
    }

    // Set up decompression if needed
    auto decompStream = std::make_unique<boost::iostreams::filtering_istream>();
    if (compressionLevel > 0)
    {
        JLOG(context.j.info())
            << "Setting up decompression with level " << (int)compressionLevel;
        boost::iostreams::zlib_params params((int)compressionLevel);
        params.window_bits = 15;
        params.noheader = false;
        decompStream->push(boost::iostreams::zlib_decompressor(params));
    }
    else
    {
        JLOG(context.j.info())
            << "No decompression needed (level 0), using direct input";
    }
    decompStream->push(boost::ref(infile));

    uint32_t ledgersLoaded = 0;
    std::shared_ptr<Ledger> prevLedger;
    uint32_t expected_seq = header.min_ledger;

    // Process each ledger sequentially
    while (!decompStream->eof() && expected_seq <= header.max_ledger)
    {
        if (context.app.isStopping())
            return {};

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
            JLOG(context.j.warn())
                << "Catalogue load expected but could not "
                << "read the next ledger header at seq=" << expected_seq << ". "
                << "Ledgers prior to this in the file (if any) were loaded.";
            return rpcError(rpcINTERNAL, "Unexpected end of catalogue file.");
        }

        info.closeTime = time_point{duration{closeTime}};
        info.parentCloseTime = time_point{duration{parentCloseTime}};
        info.closeTimeResolution = duration{closeTimeResolution};
        info.drops = drops;

        JLOG(context.j.info()) << "Found ledger " << info.seq << "...";

        if (info.seq != expected_seq++)
        {
            JLOG(context.j.error())
                << "Expected ledger " << expected_seq << ", bailing";
            return rpcError(
                rpcINTERNAL,
                "Unexpected ledger out of sequence in catalogue file");
        }

        // Create a ledger object
        std::shared_ptr<Ledger> ledger;

        if (info.seq == header.min_ledger)
        {
            // Base ledger - create a fresh one
            ledger = std::make_shared<Ledger>(
                info.seq,
                info.closeTime,
                context.app.config(),
                context.app.getNodeFamily());

            ledger->setLedgerInfo(info);

            // Deserialize the complete state map from leaf nodes
            if (!ledger->stateMap().deserializeFromStream(*decompStream))
            {
                JLOG(context.j.error())
                    << "Failed to deserialize base ledger state";
                return rpcError(
                    rpcINTERNAL, "Failed to load base ledger state");
            }
        }
        else
        {
            // Delta ledger - start with a copy of the previous ledger
            if (!prevLedger)
            {
                JLOG(context.j.error()) << "Missing previous ledger for delta";
                return rpcError(rpcINTERNAL, "Missing previous ledger");
            }

            auto snapshot = prevLedger->stateMap().snapShot(true);

            ledger = std::make_shared<Ledger>(
                info,
                context.app.config(),
                context.app.getNodeFamily(),
                *snapshot);

            // Apply delta (only leaf-node changes)
            if (!ledger->stateMap().deserializeFromStream(*decompStream))
            {
                JLOG(context.j.error())
                    << "Failed to apply delta to ledger " << info.seq;
                return rpcError(rpcINTERNAL, "Failed to apply ledger delta");
            }
        }

        // pull in the tx map
        if (!ledger->txMap().deserializeFromStream(*decompStream))
        {
            JLOG(context.j.error())
                << "Failed to apply delta to ledger " << info.seq;
            return rpcError(rpcINTERNAL, "Failed to apply ledger delta");
        }

        // Finalize the ledger
        ledger->stateMap().flushDirty(hotACCOUNT_NODE);
        ledger->txMap().flushDirty(hotTRANSACTION_NODE);

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
            JLOG(context.j.error())
                << "Ledger seq=" << info.seq
                << " was loaded from catalogue, but computed hash does not "
                   "match. "
                << "This ledger was not saved, and ledger loading from this "
                   "catalogue file ended here.";
            return rpcError(
                rpcINTERNAL, "Catalogue file contains a corrupted ledger.");
        }

        // Save in database
        pendSaveValidated(context.app, ledger, false, false);

        // Store in ledger master
        context.app.getLedgerMaster().storeLedger(ledger, true);

        if (info.seq == header.max_ledger &&
            context.app.getLedgerMaster().getClosedLedger()->info().seq <
                info.seq)
        {
            // Set as current ledger if this is the latest
            context.app.getLedgerMaster().switchLCL(ledger);
        }

        context.app.getLedgerMaster().setLedgerRangePresent(
            header.min_ledger, info.seq, true);

        // Store the ledger
        prevLedger = ledger;
        ledgersLoaded++;
    }

    decompStream->reset();
    infile.close();

    JLOG(context.j.info()) << "Catalogue load complete! Loaded "
                           << ledgersLoaded << " ledgers from file size "
                           << file_size << " bytes";

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

    return jvResult;
}

}  // namespace ripple
