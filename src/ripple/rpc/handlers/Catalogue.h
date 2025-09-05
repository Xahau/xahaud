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

#include <ripple/app/main/Application.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/core/JobTypes.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
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

// Job type for catalogue runtime status
enum class CatalogueStatusJobType { CREATE, LOAD };

// Runtime status for catalogue operations
struct CatalogueRunStatus
{
    bool isRunning = false;
    std::chrono::system_clock::time_point started;
    uint32_t minLedger;
    uint32_t maxLedger;
    uint32_t ledgerUpto;
    CatalogueStatusJobType jobType;
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

// Forward declarations
class Config;
class JobQueue;
class CollectorManager;
class Logs;
class PerfLog;

// Catalogue-specific job types for queued work
enum class CatalogueJobType {
    FLUSH_SAVE,  // Flush nodes + verify hashes + setImmutable
    SQL_SAVE     // Save ledger metadata to SQLite
};

/**
 * JobQueueAdapter - Smart JobQueue wrapper for catalogue operations
 *
 * In standalone mode: Creates and manages its own JobQueue
 * In production mode: Delegates to the application's JobQueue with polite
 * priorities
 *
 * This allows catalogue operations to use aggressive parallelism in
 * standalone/testing while being polite on production servers.
 *
 * TODO: SMART SCHEDULING IMPROVEMENTS
 * ------------------------------------
 * The adapter could implement smarter scheduling to avoid competing with
 * critical ledger operations:
 *
 * 1. **Wait for idle slots**: Check getJobCount() for critical job types
 *    (jtPUBLEDGER, jtACCEPT, jtWRITE) and only schedule catalogue jobs
 *    when those queues are empty or below threshold.
 *
 * 2. **Chain jobs with dependencies**: Use a "completion callback" pattern
 *    where SQL_SAVE jobs only get queued after their corresponding
 *    FLUSH_SAVE completes, reducing concurrent load.
 *
 * 3. **Custom JobTypes**: Add dedicated catalogue job types to JobTypes.h:
 *    - jtCATALOGUE_FLUSH (priority between jtWRITE and jtGENERIC)
 *    - jtCATALOGUE_SQL (lowest priority, bulk operations)
 *    This would give fine-grained control without hijacking existing types.
 *
 * 4. **Backpressure handling**: If catalogue jobs back up, pause the main
 *    thread's ledger processing to prevent unbounded queue growth.
 *
 * 5. **WAL Checkpoint Management**: Implement smarter WAL handling during
 *    catalogue operations:
 *    - Increase WAL size limits during bulk loads (esp. for transaction.db)
 *    - Coordinate checkpoint timing with job scheduling gaps
 *    - Consider different WAL strategies for ledger.db vs transaction.db
 *    - Auto-adjust checkpoint frequency based on write throughput
 *    The transaction database sees much higher write volume and would
 *    benefit most from tuned checkpoint behavior.
 *
 * 6. **addOrRunJob() vs addJob() - Work Distribution**:
 *    Two methods for different contexts:
 *
 *    addOrRunJob(type, name, lambda):
 *    - Used by main thread
 *    - If queue full: Execute immediately
 *    - Otherwise: Queue for background
 *
 *    addJob(type, name, lambda):
 *    - Used by job threads
 *    - Always queues and returns
 *
 *    Prevents recursive work-stealing while allowing main thread
 *    to help with backpressure
 *
 * TRADEOFFS:
 * - More complex scheduling logic vs simpler current implementation
 * - Risk of starvation if server is consistently busy
 * - Main thread backpressure could impact overall throughput
 *
 * Current approach uses existing job priorities which is "good enough"
 * but custom job types would be cleaner long-term.
 */
class JobQueueAdapter
{
public:
    JobQueueAdapter(
        Application& app,
        beast::Journal journal,
        bool forceStandalone = false);

    ~JobQueueAdapter();

    // Submit job - always queues (used by job threads)
    template <typename JobHandler>
    bool
    addJob(
        CatalogueJobType catType,
        std::string const& name,
        JobHandler&& jobHandler)
    {
        JobType jobType = mapToJobType(catType);

        if (isStandalone_ && ownQueue_)
        {
            // Use our own queue in standalone mode with aggressive priorities
            return ownQueue_->addJob(
                jobType, name, std::forward<JobHandler>(jobHandler));
        }
        else
        {
            // In production, use polite priorities
            JobType politeType = mapToPoliteJobType(catType);
            return app_.getJobQueue().addJob(
                politeType, name, std::forward<JobHandler>(jobHandler));
        }
    }

    // Job execution result enum
    enum class JobResult {
        QUEUED,    // Job was successfully queued
        EXECUTED,  // Job was executed inline
        SKIPPED    // Job was skipped due to limits
    };

    // Submit or run job - executes immediately if queue full (used by main
    // thread)
    template <typename JobHandler>
    bool
    addOrRunJob(
        CatalogueJobType catType,
        std::string const& name,
        JobHandler&& jobHandler)
    {
        // Check per-type concurrent limits first
        int currentConcurrent = 0;
        std::atomic<int>* counter = nullptr;

        switch (catType)
        {
            case CatalogueJobType::FLUSH_SAVE:
                counter = &concurrentFlushJobs_;
                currentConcurrent = concurrentFlushJobs_.load();
                break;
            case CatalogueJobType::SQL_SAVE:
                counter = &concurrentSQLJobs_;
                currentConcurrent = concurrentSQLJobs_.load();
                break;
            default:
                break;
        }

        int maxConcurrent = getMaxConcurrent(catType);

        // If we're at the concurrent limit for this type, execute inline
        if (counter && currentConcurrent >= maxConcurrent)
        {
            JLOG(j_.info()) << "Type limit reached (" << currentConcurrent
                            << "/" << maxConcurrent << "), executing " << name
                            << " in MAIN THREAD (forced inline)";
            jobHandler();
            return true;
        }

        // Increment counter for this job type
        if (counter)
            (*counter)++;

        // Wrap the handler to decrement counter when done
        auto wrappedHandler = [this,
                               jobHandler =
                                   std::forward<JobHandler>(jobHandler),
                               counter,
                               catType]() mutable {
            jobHandler();
            if (counter)
                (*counter)--;
        };

        // Check queue capacity and execute immediately if full
        if (isStandalone_ && ownQueue_)
        {
            JobType jobType = mapToJobType(catType);

            // We want at most thread_count jobs queued
            // This keeps workers busy without building a huge backlog
            int waitingJobs = ownQueue_->getJobCount(jobType);
            int threadCount = calculateOptimalThreads(app_.config());

            if (waitingJobs >= threadCount)
            {
                // Queue is full, execute synchronously in main thread
                JLOG(j_.debug())
                    << "Queue full (" << waitingJobs << " waiting), executing "
                    << name << " in main thread";
                wrappedHandler();
                return true;
            }

            // Queue has capacity, add job normally
            return ownQueue_->addJob(jobType, name, std::move(wrappedHandler));
        }
        else
        {
            // Production mode - check app queue capacity
            JobType politeType = mapToPoliteJobType(catType);

            // In production, be conservative - just a small buffer
            int waitingJobs = app_.getJobQueue().getJobCount(politeType);
            int threshold = 4;  // Small buffer for production

            if (waitingJobs >= threshold)
            {
                // Queue is full, execute synchronously in main thread
                JLOG(j_.debug())
                    << "Queue full (" << waitingJobs << " waiting), executing "
                    << name << " in main thread";
                wrappedHandler();
                return true;
            }

            // Queue has capacity, add job normally
            return app_.getJobQueue().addJob(
                politeType, name, std::move(wrappedHandler));
        }
    }

    // Stop the queue (only affects owned queue)
    void
    stop();

    // Get optimal thread count for catalogue operations
    static int
    calculateOptimalThreads(Config const& config);

private:
    // Maximum concurrent jobs per type (to avoid SQLite contention)
    static int
    getMaxConcurrent(CatalogueJobType catType)
    {
        // Tunable: Adjust this to control SQLite contention
        static constexpr int MAX_CONCURRENT_SQL =
            16;  // FLOOD MODE: Maximum parallelism!

        switch (catType)
        {
            case CatalogueJobType::FLUSH_SAVE:
                return MAX_CONCURRENT_SQL;  // Allow 16 flush jobs concurrently

            case CatalogueJobType::SQL_SAVE:
                return MAX_CONCURRENT_SQL;  // Allow 16 SQL jobs (if used
                                            // separately)

            default:
                return MAX_CONCURRENT_SQL;  // Default to same limit
        }
    }

    // Map catalogue job types to standard job types (standalone mode)
    static JobType
    mapToJobType(CatalogueJobType catType)
    {
        switch (catType)
        {
            case CatalogueJobType::FLUSH_SAVE:
                return jtWRITE;  // High priority for flush operations

            case CatalogueJobType::SQL_SAVE:
                return jtPUBOLDLEDGER;  // Medium priority for SQL

            default:
                return jtGENERIC;
        }
    }

    // Map catalogue job types to polite priorities (production mode)
    static JobType
    mapToPoliteJobType(CatalogueJobType catType)
    {
        switch (catType)
        {
            case CatalogueJobType::FLUSH_SAVE:
                return jtPUBOLDLEDGER;  // Medium priority, don't hog resources

            case CatalogueJobType::SQL_SAVE:
                return jtGENERIC;  // Low priority to avoid SQL contention

            default:
                return jtGENERIC;
        }
    }

    Application& app_;
    beast::Journal j_;
    bool isStandalone_;
    std::unique_ptr<JobQueue> ownQueue_;

    // Track concurrent jobs per catalogue type
    std::atomic<int> concurrentFlushJobs_{0};
    std::atomic<int> concurrentSQLJobs_{0};
};

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