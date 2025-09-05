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
#include <ripple/app/main/Application.h>
#include <ripple/app/main/CollectorManager.h>
#include <ripple/app/misc/SHAMapStoreImp.h>
#include <ripple/app/rdb/backend/SQLiteDatabase.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/RangeSet.h>
#include <ripple/beast/core/CurrentThreadName.h>
#include <ripple/core/JobQueue.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/LedgerFormats.h>
#include <ripple/protocol/digest.h>
#include <ripple/rpc/handlers/Catalogue.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <condition_variable>
#include <fstream>
#include <future>
#include <mutex>
#include <queue>
#include <sys/stat.h>
#include <thread>

namespace ripple {

/**
 * catalogue_load: Live Server Ledger Range Expansion ONLY
 *
 * ARCHITECTURAL DECISION:
 * catalogue_load is EXCLUSIVELY for expanding ledger ranges on live servers.
 * For new node initialization, use dedicated standalone tools that can be
 * aggressive with parallelism and bypass rippled's abstractions entirely.
 *
 * USE CASE: Live server needs to expand its ledger range without downtime.
 * - Must be "nice" to existing P2P sync, RPC serving, and consensus
 * - Must work within SQLite/NuDB single-process write constraints
 * - Must not overwhelm the server or cause service degradation
 *
 * SIMPLIFIED SINGLE-THREADED DESIGN:
 * After extensive experimentation with parallel processing, we've returned to
 * enlightened single-threading because:
 * - SHAMap's COW architecture fundamentally requires serial ledger building
 * - SQLite lock contention from parallel writes caused more harm than good
 * - Complex threading infrastructure wasn't worth the minimal speedup
 * - Being "polite" is more important than raw speed for live expansion
 *
 * EXECUTION FLOW:
 * 1. Deserialize ledger from catalogue file (main thread)
 * 2. Canonicalize state map via unshare() for COW safety (main thread)
 * 3. Flush nodes to disk using efficient pointer diff (main thread)
 * 4. Queue SQL save as jtPUBOLDLEDGER job (yields to P2P operations)
 * 5. Wait for SQL job completion before processing next ledger
 *
 * POLITENESS STRATEGY:
 * - SQL saves queued as jtPUBOLDLEDGER (medium priority)
 * - Automatically yields to P2P ledger acquisition and consensus
 * - Synchronous waiting prevents queue flooding
 * - Natural pacing allows WAL checkpoints and cache management
 *
 * PERFORMANCE EXPECTATION:
 * "Good enough" for occasional range expansion. NOT optimized for bulk loading.
 * Typical use: Expanding history on a validator or adding missing ranges.
 *
 * FOR NEW NODE SETUP:
 * Use a dedicated standalone tool that can:
 * - Run parallel I/O operations aggressively
 * - Use bulk SQL operations and prepared statements
 * - Bypass SHAMap COW complexity entirely
 * - Tune WAL mode, page cache, and checkpoint behavior
 */

Json::Value
doCatalogueLoad(RPC::JsonContext& context)
{
    auto j = context.app.logs().journal("CatalogueTools");
    auto statusJournal = context.app.logs().journal("CatalogueToolsStatus");

    // Check if online_delete is configured - catalogue loading is incompatible
    // with online_delete. The proper workflow is:
    // 1. Load catalogues WITHOUT online_delete to prepare a snapshot
    // 2. Configure online_delete with the shell command to use that snapshot
    // 3. Run normally with the pinned ranges
    auto& shaMapStore =
        dynamic_cast<SHAMapStoreImp&>(context.app.getSHAMapStore());
    // TODO: we should remove this check, or move the method to the base class
    // We probably don't actually even need this once we've got a new
    // PinnedDatabase that layers RWDB on top of NuDB
    if (shaMapStore.isOnlineDeleteEnabled())
    {
        JLOG(j.warn()) << "TODO: catalogue_load is incompatible with "
                          "online_delete unless using pinned_type. ";
        // return rpcError(
        //     rpcINVALID_PARAMS,
        //     "catalogue_load is incompatible with online_delete. "
        //     "Please disable online_delete in the configuration, load "
        //     "catalogues "
        //     "to prepare a snapshot, then configure online_delete to use that
        //     " "snapshot.");
    }

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

    bool do_pinning = true;

    // Parse pinning option if provided
    if (context.params.isMember(jss::pin))
        do_pinning = context.params[jss::pin].asBool();

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

    // Initialize status tracking
    {
        std::unique_lock<std::shared_mutex> writeLock(catalogueStatusMutex);
        catalogueRunStatus.isRunning = true;
        catalogueRunStatus.started = std::chrono::system_clock::now();
        catalogueRunStatus.minLedger = header.min_ledger;
        catalogueRunStatus.maxLedger = header.max_ledger;
        catalogueRunStatus.ledgerUpto =
            0;  // Initialize to 0 to indicate no progress yet
        catalogueRunStatus.jobType = CatalogueStatusJobType::LOAD;
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

    if (header.network_id != context.app.config().NETWORK_ID)
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

    // Verify hash if not ignored
    if (!ignore_hash && file_size > sizeof(CATLHeader))
    {
        JLOG(j.info()) << "Verifying catalogue hash...";

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
            JLOG(j.error())
                << "Catalogue hash verification failed. Expected: " << hash_hex
                << ", Computed: " << computed_hex;
            return rpcError(
                rpcINVALID_PARAMS, "catalogue hash verification failed");
        }

        JLOG(j.info()) << "Catalogue hash verified successfully";

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
        JLOG(j.info()) << "Setting up decompression with level "
                       << (int)compressionLevel;
        boost::iostreams::zlib_params params((int)compressionLevel);
        params.window_bits = 15;
        params.noheader = false;
        decompStream->push(boost::iostreams::zlib_decompressor(params));
    }
    else
    {
        JLOG(j.info())
            << "No decompression needed (level 0), using direct input";
    }

    // Add byte counter BEFORE the final device (file)
    // This will track bytes read from the decompressed stream
    ByteCounterInputFilter byteCounter(&catalogueRunStatus.fileBytesProcessed);
    decompStream->push(byteCounter);

    decompStream->push(boost::ref(infile));

    // Track SQL performance metrics
    struct SQLMetrics
    {
        uint64_t totalJobs = 0;
        uint64_t totalMicroseconds = 0;
        uint64_t lastIntervalJobs = 0;
        uint64_t lastIntervalMicroseconds = 0;
        uint64_t totalQueueWaitMicroseconds =
            0;                         // Total time waiting for promises
        uint64_t totalQueueWaits = 0;  // Number of times we waited
        uint64_t lastIntervalQueueWaitMicroseconds = 0;
        uint64_t lastIntervalQueueWaits = 0;
        std::chrono::steady_clock::time_point startTime;

        // Rolling 1-second averages for SQL execution times (max 600 = 10
        // minutes)
        struct SecondBucket
        {
            uint64_t totalMicros = 0;
            uint32_t count = 0;

            void
            add(uint64_t microseconds)
            {
                totalMicros += microseconds;
                count++;
            }

            double
            getAvgMs() const
            {
                return count > 0 ? (totalMicros / 1000.0) / count : 0;
            }

            void
            reset()
            {
                totalMicros = 0;
                count = 0;
            }
        };

        std::deque<SecondBucket> secondBuckets;
        SecondBucket currentSecond;
        std::chrono::steady_clock::time_point currentSecondStart;
        const size_t maxBuckets = 600;  // 10 minutes of 1-second buckets

        SQLMetrics()
            : startTime(std::chrono::steady_clock::now())
            , currentSecondStart(std::chrono::steady_clock::now())
        {
        }

        void
        addSample(uint64_t microseconds)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               now - currentSecondStart)
                               .count();

            // If we've moved to a new second, save the current bucket
            if (elapsed >= 1)
            {
                if (currentSecond.count > 0)
                {
                    secondBuckets.push_back(currentSecond);
                    if (secondBuckets.size() > maxBuckets)
                    {
                        secondBuckets.pop_front();
                    }
                }
                currentSecond.reset();
                currentSecondStart = now;
            }

            currentSecond.add(microseconds);
        }

        double
        getWindowAvgMs(int seconds) const
        {
            int bucketCount =
                std::min(seconds, static_cast<int>(secondBuckets.size()));
            if (bucketCount == 0)
            {
                // Only have current second
                return currentSecond.getAvgMs();
            }

            uint64_t totalMicros = currentSecond.totalMicros;
            uint32_t totalCount = currentSecond.count;

            // Add the most recent N buckets
            auto it = secondBuckets.rbegin();
            for (int i = 0; i < bucketCount && it != secondBuckets.rend();
                 ++i, ++it)
            {
                totalMicros += it->totalMicros;
                totalCount += it->count;
            }

            return totalCount > 0 ? (totalMicros / 1000.0) / totalCount : 0;
        }
    };
    auto sqlMetrics = std::make_shared<SQLMetrics>();

    // SIMPLIFIED SINGLE-THREADED APPROACH:
    // - Everything runs synchronously in the main thread
    // - No JobQueueAdapter needed
    // - No concurrent jobs or work-stealing
    // - Standalone mode "just works" automatically
    // - Avoids fighting with SHAMap's COW architecture
    // - Good enough performance for expanding live server ranges

    static constexpr int BATCH_UPDATE_INTERVAL =
        100;  // Update ranges every N ledgers

    // SIMPLIFIED SINGLE-THREADED EXECUTION:
    // - Everything runs synchronously in the main thread
    // - Build ledger -> Canonicalize -> Flush -> SQL Save
    // - No parallelism, no JobQueueAdapter, no complexity
    // - "Good enough" performance for live expansion
    // - Avoids fighting SHAMap's COW architecture

    uint32_t ledgersLoaded = 0;
    std::shared_ptr<Ledger> prevLedger;
    uint32_t expected_seq = header.min_ledger;
    RangeSet<uint32_t> completedSaves;

    // Buffer for batch saving ledgers
    std::vector<std::shared_ptr<Ledger const>> ledgerBuffer;
    static constexpr size_t BATCH_SIZE = 500;  // Save 500 ledgers at a time

    // Process ledgers sequentially
    while (!decompStream->eof() && expected_seq <= header.max_ledger)
    {
        if (context.app.isStopping())
            return {};

        // With canonicalization, no synchronization needed!
        // The unshare() call makes state map nodes immutable (cowid=0),
        // allowing safe concurrent access from multiple background threads.

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
                << "read the next ledger header at seq=" << expected_seq << ". "
                << "Ledgers prior to this in the file (if any) were loaded.";
            return rpcError(rpcINTERNAL, "Unexpected end of catalogue file.");
        }

        using time_point = NetClock::time_point;
        using duration = NetClock::duration;

        info.closeTime = time_point{duration{closeTime}};
        info.parentCloseTime = time_point{duration{parentCloseTime}};
        info.closeTimeResolution = duration{closeTimeResolution};
        info.drops = drops;

        JLOG(j.info()) << "Found ledger " << info.seq << "...";

        if (info.seq != expected_seq++)
        {
            JLOG(j.error())
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
            if (!ledger->stateMap().deserializeFromStream(
                    *decompStream, pinnedACCOUNT_NODE))
            {
                JLOG(j.error()) << "Failed to deserialize base ledger state";
                return rpcError(
                    rpcINTERNAL, "Failed to load base ledger state");
            }

            // Don't flush here - will be done in background after setImmutable
        }
        else
        {
            // Delta ledger - start with a copy of the previous ledger
            if (!prevLedger)
            {
                JLOG(j.error()) << "Missing previous ledger for delta";
                return rpcError(rpcINTERNAL, "Missing previous ledger");
            }

            // auto snapshot = prevLedger->stateMap().snapShot(true);

            ledger = std::make_shared<Ledger>(
                info,
                context.app.config(),
                context.app.getNodeFamily(),
                prevLedger->stateMap());

            // Apply delta (only leaf-node changes)
            if (!ledger->stateMap().deserializeFromStream(
                    *decompStream, pinnedACCOUNT_NODE))
            {
                JLOG(j.error())
                    << "Failed to apply delta to ledger " << info.seq;
                return rpcError(rpcINTERNAL, "Failed to apply ledger delta");
            }

            // Don't flush here - will be done in background after setImmutable
        }

        // pull in the tx map
        if (!ledger->txMap().deserializeFromStream(
                *decompStream, pinnedTRANSACTION_NODE))
        {
            JLOG(j.error()) << "Failed to apply delta to ledger " << info.seq;
            return rpcError(rpcINTERNAL, "Failed to apply ledger delta");
        }

        // CANONICALIZE in main thread for thread safety
        // Canonicalize state map for COW safety
        // unshare() calls walkSubTree(false, hotUNKNOWN) internally
        // This computes hashes and marks nodes clean (cowid=0)
        JLOG(j.trace()) << "Canonicalizing state map for ledger " << info.seq
                        << " in main thread...";

        ledger->stateMap().unshare();

        // Verify STATE hash after canonicalization
        auto computedStateHash = ledger->stateMap().getHash().as_uint256();
        if (computedStateHash != info.accountHash)
        {
            JLOG(j.error()) << "State hash mismatch for ledger " << info.seq
                            << " Expected: " << info.accountHash
                            << " Got: " << computedStateHash;
            return rpcError(
                rpcINTERNAL, "State hash mismatch after canonicalization");
        }

        JLOG(j.trace()) << "Ledger " << info.seq
                        << " state map canonicalized and verified";

        // SINGLE-THREADED PROCESSING: Flush and save directly
        {
            JLOG(j.trace()) << "Processing ledger " << ledger->seq()
                            << " hash: " << ledger->info().hash;

            // Flush nodes using pointer diff for efficiency
            int stateNodesFlushed = 0;
            int txNodesFlushed = 0;

            JLOG(j.trace()) << "Flushing ledger " << ledger->info().seq
                            << " (state canonical, tx dirty)";

            // Use pointer diff for state map (efficient delta flushing)
            JLOG(j.trace())
                << "Using flushByPointerDiff for ledger " << ledger->info().seq
                << (prevLedger ? " (with parent)" : " (first ledger)");

            stateNodesFlushed = ledger->stateMap().flushByPointerDiff(
                prevLedger
                    ? std::optional<std::reference_wrapper<const SHAMap>>(
                          prevLedger->stateMap())
                    : std::nullopt,
                pinnedACCOUNT_NODE);

            JLOG(j.trace()) << "State map flushed " << stateNodesFlushed
                            << " nodes for ledger " << ledger->info().seq;

            // TX map - use flushDirty since TX maps are unique per ledger
            txNodesFlushed = ledger->txMap().flushDirty(pinnedTRANSACTION_NODE);

            JLOG(j.trace()) << "TX map flushed " << txNodesFlushed
                            << " nodes for ledger " << ledger->info().seq;

            // Verify TX hash after flushing
            auto computedTxHash = ledger->txMap().getHash().as_uint256();
            if (computedTxHash != info.txHash)
            {
                JLOG(j.error())
                    << "TX hash mismatch for ledger " << ledger->info().seq
                    << " Expected: " << info.txHash
                    << " Got: " << computedTxHash;
                return rpcError(
                    rpcINTERNAL,
                    "TX hash mismatch for ledger " +
                        std::to_string(ledger->info().seq));
            }

            // Finalize the ledger
            ledger->setAccepted(
                info.closeTime,
                info.closeTimeResolution,
                info.closeFlags & sLCF_NoConsensusTime);
            ledger->setValidated();
            ledger->setCloseFlags(info.closeFlags);

            // Make the ledger immutable
            ledger->setImmutable(true);

            // Verify the hash matches what was expected
            if (ledger->info().hash != info.hash)
            {
                JLOG(j.error())
                    << "Ledger seq=" << ledger->info().seq
                    << " hash mismatch after flush! Expected: " << info.hash
                    << " Got: " << ledger->info().hash;
                return rpcError(
                    rpcINTERNAL,
                    "Catalogue file contains a corrupted ledger at sequence " +
                        std::to_string(ledger->info().seq));
            }

            // Add ledger to buffer for batch saving
            ledgerBuffer.push_back(ledger);

            // Save batch when buffer is full or we're at the last ledger
            if (ledgerBuffer.size() >= BATCH_SIZE ||
                expected_seq > header.max_ledger)
            {
                // Save to SQLite
                // In standalone mode: execute directly (no need to be polite)
                // In production mode: queue through JobQueue to yield to P2P
                // operations
                {
                    // TESTING: Skip SQLite entirely to see max throughput
                    bool SKIP_SQL = false;

                    bool success = false;

                    if (SKIP_SQL)
                    {
                        success = true;
                        JLOG(j.trace()) << "SKIPPING SQL for "
                                        << ledgerBuffer.size() << " ledgers";
                    }
                    else
                    {
                        if (context.app.config().standalone())
                        {
                            // Standalone mode - just execute directly
                            auto start = std::chrono::steady_clock::now();

                            auto const db = dynamic_cast<SQLiteDatabase*>(
                                &context.app.getRelationalDatabase());
                            if (!db)
                            {
                                JLOG(j.error())
                                    << "Failed to get database for batch save";
                                return rpcError(
                                    rpcINTERNAL, "Failed to get database");
                            }

                            // Save the entire batch at once!
                            success = db->saveValidatedLedgers(ledgerBuffer);

                            auto end = std::chrono::steady_clock::now();
                            auto duration =
                                std::chrono::duration_cast<
                                    std::chrono::microseconds>(end - start)
                                    .count();

                            sqlMetrics->totalJobs++;  // One batch job
                            sqlMetrics->totalMicroseconds += duration;
                            sqlMetrics->lastIntervalJobs++;
                            sqlMetrics->lastIntervalMicroseconds += duration;

                            // Add sample for the actual batch duration
                            sqlMetrics->addSample(duration);

                            if (success)
                            {
                                JLOG(j.trace())
                                    << "SQL: Successfully saved batch of "
                                    << ledgerBuffer.size() << " ledgers in "
                                    << (duration / 1000.0) << " ms";
                            }
                            else
                            {
                                JLOG(j.error()) << "Failed to save batch of "
                                                << ledgerBuffer.size()
                                                << " ledgers to SQLite";
                            }
                        }
                        else
                        {
                            // Production mode - use JobQueue to be polite
                            std::promise<bool> sqlPromise;
                            auto sqlFuture = sqlPromise.get_future();

                            // Capture the batch for the job
                            auto batchToSave = ledgerBuffer;

                            auto sqlJob = [&context,
                                           batchToSave,
                                           sqlMetrics,
                                           j,
                                           sqlPromisePtr = &sqlPromise]() {
                                auto start = std::chrono::steady_clock::now();

                                auto const db = dynamic_cast<SQLiteDatabase*>(
                                    &context.app.getRelationalDatabase());
                                if (!db)
                                {
                                    JLOG(j.error()) << "Failed to get database "
                                                       "for batch save";
                                    sqlPromisePtr->set_value(false);
                                    return;
                                }

                                // Save the entire batch at once!
                                bool success =
                                    db->saveValidatedLedgers(batchToSave);

                                auto end = std::chrono::steady_clock::now();
                                auto duration =
                                    std::chrono::duration_cast<
                                        std::chrono::microseconds>(end - start)
                                        .count();

                                sqlMetrics->totalJobs++;  // One batch job
                                sqlMetrics->totalMicroseconds += duration;
                                sqlMetrics->lastIntervalJobs++;
                                sqlMetrics->lastIntervalMicroseconds +=
                                    duration;

                                // Add sample for the actual batch duration
                                sqlMetrics->addSample(duration);

                                if (success)
                                {
                                    JLOG(j.trace())
                                        << "SQL: Successfully saved batch of "
                                        << batchToSave.size() << " ledgers in "
                                        << (duration / 1000.0) << " ms";
                                }
                                else
                                {
                                    JLOG(j.error())
                                        << "Failed to save batch of "
                                        << batchToSave.size()
                                        << " ledgers to SQLite";
                                }

                                sqlPromisePtr->set_value(success);
                            };

                            // Queue as jtPUBOLDLEDGER - medium priority that
                            // waits behind P2P operations
                            context.app.getJobQueue().addJob(
                                jtPUBOLDLEDGER,
                                "catalogue-sql-" +
                                    std::to_string(ledger->info().seq),
                                std::move(sqlJob));

                            // Track how long we wait for the promise
                            auto waitStart = std::chrono::steady_clock::now();

                            // Wait for SQL save to complete
                            success = sqlFuture.get();

                            auto waitEnd = std::chrono::steady_clock::now();
                            auto waitDuration = std::chrono::duration_cast<
                                                    std::chrono::microseconds>(
                                                    waitEnd - waitStart)
                                                    .count();

                            sqlMetrics->totalQueueWaitMicroseconds +=
                                waitDuration;
                            sqlMetrics->totalQueueWaits++;
                            sqlMetrics->lastIntervalQueueWaitMicroseconds +=
                                waitDuration;
                            sqlMetrics->lastIntervalQueueWaits++;
                        }
                    }  // end if (!SKIP_SQL)

                    if (!success)
                    {
                        // Build error message with ledger range
                        std::string errorMsg = "Failed to save batch of " +
                            std::to_string(ledgerBuffer.size()) + " ledgers (";
                        if (!ledgerBuffer.empty())
                        {
                            errorMsg += std::to_string(
                                            ledgerBuffer.front()->info().seq) +
                                "-" +
                                std::to_string(ledgerBuffer.back()->info().seq);
                        }
                        errorMsg += ") to SQLite database";
                        return rpcError(rpcINTERNAL, errorMsg);
                    }

                    // Mark all ledgers in the batch as saved for range tracking
                    for (auto const& savedLedger : ledgerBuffer)
                    {
                        completedSaves.insert(savedLedger->info().seq);
                    }

                    // Clear the buffer after successful save
                    ledgerBuffer.clear();
                }
            }

            // Log periodically for debugging
            if (ledger->info().seq % 1000 == 0)
            {
                JLOG(j.info())
                    << "Processed ledger " << ledger->info().seq << ": "
                    << stateNodesFlushed << " state nodes, " << txNodesFlushed
                    << " tx nodes "
                    << (prevLedger ? "(delta flush)" : "(full flush)");
            }
        }

        // Store in ledger master
        // IMPORTANT: When do_pinning is true (default), storeLedger
        // deliberately does NOT insert the ledger into mLedgerHistory to avoid
        // memory bloat. With millions of ledgers, keeping them all in the
        // history cache would prevent garbage collection of SHAMap nodes,
        // causing massive memory usage. We only mark the ledger as pinned for
        // persistence purposes.
        context.app.getLedgerMaster().storeLedger(ledger, do_pinning);

        // Store the ledger reference for later use
        prevLedger = ledger;
        ledgersLoaded++;

        // In standalone mode, log status JSON periodically (every 2 seconds)
        // This gives better visibility into processing rhythm than ledger-based
        // intervals
        static auto lastStatusLog = std::chrono::steady_clock::now();
        static auto loadStartTime = std::chrono::steady_clock::now();
        static uint32_t lastLedgerCount = 0;
        static uint64_t lastBytesProcessed = 0;
        static uint64_t totalTxnCount = 0;
        static uint64_t lastTxnCount = 0;

        // Count transactions in this ledger
        auto txCount = std::distance(ledger->txs.begin(), ledger->txs.end());
        totalTxnCount += txCount;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastStatusLog);

        auto logStatus = true;

        if ((context.app.config().standalone() || logStatus) &&
            elapsed.count() >= 1000)  // Log every 1 second to catch rhythm
        {
            std::shared_lock<std::shared_mutex> lock(catalogueStatusMutex);

            // Calculate performance metrics for this interval
            uint32_t currentLedgerCount = ledgersLoaded;
            uint64_t currentBytesProcessed =
                catalogueRunStatus.fileBytesProcessed.load();

            uint32_t ledgersDelta = currentLedgerCount - lastLedgerCount;
            uint64_t bytesDelta = currentBytesProcessed - lastBytesProcessed;
            uint64_t txnsDelta = totalTxnCount - lastTxnCount;

            double secondsElapsed = elapsed.count() / 1000.0;
            double ledgersPerSec = ledgersDelta / secondsElapsed;
            double bytesPerSec = bytesDelta / secondsElapsed;
            double txnsPerSec = txnsDelta / secondsElapsed;

            // Calculate overall averages since start
            auto totalElapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - loadStartTime)
                    .count() /
                1000.0;
            double avgTxnsPerSec = totalTxnCount / totalElapsed;
            double avgBytesPerSec = currentBytesProcessed / totalElapsed;

            // Create JSON with status and performance sections
            Json::Value outputJson(Json::objectValue);
            outputJson["status"] = generateStatusJson();

            Json::Value perfJson(Json::objectValue);
            perfJson["interval_seconds"] = secondsElapsed;
            perfJson["ledgers_in_interval"] = ledgersDelta;
            perfJson["ledgers_per_sec"] =
                std::round(ledgersPerSec * 10) / 10;  // 1 decimal place
            perfJson["txns_in_interval"] = static_cast<Json::UInt>(txnsDelta);
            perfJson["txns_per_sec"] =
                std::round(txnsPerSec * 10) / 10;  // 1 decimal place
            perfJson["txns_per_sec_avg"] =
                std::round(avgTxnsPerSec * 10) / 10;  // Overall average
            perfJson["bytes_in_interval"] = formatBytesIEC(bytesDelta);
            perfJson["bytes_per_sec"] =
                formatBytesIEC(static_cast<uint64_t>(bytesPerSec)) + "/s";
            perfJson["bytes_per_sec_avg"] =
                formatBytesIEC(static_cast<uint64_t>(avgBytesPerSec)) +
                "/s";  // Overall average

            // Add SQL metrics
            auto sqlJobsInterval = sqlMetrics->lastIntervalJobs;
            sqlMetrics->lastIntervalJobs = 0;
            auto sqlMicrosInterval = sqlMetrics->lastIntervalMicroseconds;
            sqlMetrics->lastIntervalMicroseconds = 0;
            if (sqlJobsInterval > 0)
            {
                perfJson["sql_jobs_in_interval"] =
                    static_cast<Json::UInt>(sqlJobsInterval);
                perfJson["sql_jobs_per_sec"] =
                    std::round(sqlJobsInterval / secondsElapsed * 10) / 10;
                perfJson["sql_avg_ms"] =
                    std::round(
                        sqlMicrosInterval / 1000.0 / sqlJobsInterval * 10) /
                    10;
            }

            // SQL totals
            auto totalSQLJobs = sqlMetrics->totalJobs;
            auto totalSQLMicros = sqlMetrics->totalMicroseconds;
            if (totalSQLJobs > 0)
            {
                perfJson["sql_total_jobs"] =
                    static_cast<Json::UInt>(totalSQLJobs);
                perfJson["sql_avg_ms_overall"] =
                    std::round(totalSQLMicros / 1000.0 / totalSQLJobs * 10) /
                    10;
            }

            // Queue wait time metrics (production mode only)
            auto queueWaitsInterval = sqlMetrics->lastIntervalQueueWaits;
            sqlMetrics->lastIntervalQueueWaits = 0;
            auto queueWaitMicrosInterval =
                sqlMetrics->lastIntervalQueueWaitMicroseconds;
            sqlMetrics->lastIntervalQueueWaitMicroseconds = 0;

            if (queueWaitsInterval > 0)
            {
                perfJson["queue_waits_in_interval"] =
                    static_cast<Json::UInt>(queueWaitsInterval);
                perfJson["queue_wait_avg_ms"] =
                    std::round(
                        queueWaitMicrosInterval / 1000.0 / queueWaitsInterval *
                        10) /
                    10;
            }

            // Total queue wait metrics
            auto totalQueueWaits = sqlMetrics->totalQueueWaits;
            auto totalQueueWaitMicros = sqlMetrics->totalQueueWaitMicroseconds;
            if (totalQueueWaits > 0)
            {
                perfJson["queue_total_waits"] =
                    static_cast<Json::UInt>(totalQueueWaits);
                perfJson["queue_wait_total_seconds"] =
                    std::round(totalQueueWaitMicros / 1000000.0 * 10) / 10;
                perfJson["queue_wait_avg_ms_overall"] =
                    std::round(
                        totalQueueWaitMicros / 1000.0 / totalQueueWaits * 10) /
                    10;
            }

            // Add SQL histogram data as array to preserve ordering
            Json::Value histArray(Json::arrayValue);
            auto addHistEntry = [&histArray](const char* label, double value) {
                Json::Value entry(Json::objectValue);
                entry[label] = std::round(value * 10) / 10;
                histArray.append(entry);
            };

            addHistEntry("1s", sqlMetrics->getWindowAvgMs(1));
            addHistEntry("5s", sqlMetrics->getWindowAvgMs(5));
            addHistEntry("15s", sqlMetrics->getWindowAvgMs(15));
            addHistEntry("30s", sqlMetrics->getWindowAvgMs(30));
            addHistEntry("1m", sqlMetrics->getWindowAvgMs(60));
            addHistEntry("2m", sqlMetrics->getWindowAvgMs(120));
            addHistEntry("5m", sqlMetrics->getWindowAvgMs(300));
            addHistEntry("10m", sqlMetrics->getWindowAvgMs(600));
            perfJson["sql_hist_ms"] = histArray;

            outputJson["perf"] = perfJson;

            JLOG(statusJournal.info())
                << "Catalogue load status at ledger " << ledger->info().seq
                << ": " << outputJson.toStyledString();

            // Update tracking variables
            lastStatusLog = now;
            lastLedgerCount = currentLedgerCount;
            lastBytesProcessed = currentBytesProcessed;
            lastTxnCount = totalTxnCount;
        }

        // Periodically update ranges
        if (ledgersLoaded % BATCH_UPDATE_INTERVAL == 0)
        {
            // Update ledger ranges for all completed saves
            if (!completedSaves.empty())
            {
                // Get all ranges that have been saved
                for (auto const& interval : completedSaves)
                {
                    auto first = interval.lower();
                    auto last = interval.upper();
                    context.app.getLedgerMaster().setLedgerRangePresent(
                        first, last, do_pinning);
                    JLOG(j.info())
                        << "Updated ledger range: " << first << "-" << last;
                }
                // Clear for next batch
                completedSaves.clear();
            }

            // Save pinned ranges to database if pinning is enabled
            if (do_pinning)
            {
                JLOG(j.info()) << "Saving pinned ledger ranges to database";
                auto& shaMapStore =
                    dynamic_cast<SHAMapStoreImp&>(context.app.getSHAMapStore());
                shaMapStore.savePinnedRanges(
                    context.app.getLedgerMaster().getPinnedLedgersRangeSet());
            }

            // DISABLED: Sweep causes periodic stutters during loading
            // JLOG(j.info()) << "Sweeping NodeStore cache at ledger " <<
            // info.seq
            //                << " (loaded " << ledgersLoaded << " ledgers)";
            // context.app.getNodeStore().sweep();
        }
    }

    // Save any remaining ledgers in the buffer
    if (!ledgerBuffer.empty())
    {
        JLOG(j.info()) << "Saving final batch of " << ledgerBuffer.size()
                       << " ledgers";

        bool success = false;

        if (context.app.config().standalone())
        {
            auto start = std::chrono::steady_clock::now();

            auto const db = dynamic_cast<SQLiteDatabase*>(
                &context.app.getRelationalDatabase());
            if (!db)
            {
                return rpcError(
                    rpcINTERNAL, "Failed to get database for final batch");
            }

            success = db->saveValidatedLedgers(ledgerBuffer);

            auto end = std::chrono::steady_clock::now();
            auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    end - start)
                    .count();

            sqlMetrics->totalJobs++;  // One batch job
            sqlMetrics->totalMicroseconds += duration;

            // Add sample for the actual batch duration
            sqlMetrics->addSample(duration);

            JLOG(j.info()) << "SQL: Final batch of " << ledgerBuffer.size()
                           << " ledgers saved in " << (duration / 1000.0)
                           << " ms";
        }
        else
        {
            // Production mode - use JobQueue
            std::promise<bool> sqlPromise;
            auto sqlFuture = sqlPromise.get_future();

            auto batchToSave = ledgerBuffer;

            auto sqlJob = [&context,
                           batchToSave,
                           sqlMetrics,
                           j,
                           sqlPromisePtr = &sqlPromise]() {
                auto const db = dynamic_cast<SQLiteDatabase*>(
                    &context.app.getRelationalDatabase());
                if (!db)
                {
                    sqlPromisePtr->set_value(false);
                    return;
                }

                bool result = db->saveValidatedLedgers(batchToSave);
                sqlPromisePtr->set_value(result);
            };

            context.app.getJobQueue().addJob(
                jtPUBOLDLEDGER, "SaveFinalBatch", sqlJob);

            success = sqlFuture.get();
        }

        if (!success)
        {
            std::string errorMsg = "Failed to save final batch of " +
                std::to_string(ledgerBuffer.size()) + " ledgers";
            return rpcError(rpcINTERNAL, errorMsg);
        }

        // Mark all ledgers in the batch as saved
        for (auto const& savedLedger : ledgerBuffer)
        {
            completedSaves.insert(savedLedger->info().seq);
        }

        ledgerBuffer.clear();
    }

    decompStream->reset();
    infile.close();

    // Update ledger ranges for any remaining completed saves
    if (!completedSaves.empty())
    {
        for (auto const& interval : completedSaves)
        {
            auto first = interval.lower();
            auto last = interval.upper();
            context.app.getLedgerMaster().setLedgerRangePresent(
                first, last, do_pinning);
            JLOG(j.info()) << "Final update ledger range: " << first << "-"
                           << last;
        }
    }

    // Save pinned ranges to database if pinning was enabled
    if (do_pinning)
    {
        JLOG(j.info()) << "Saving pinned ledger ranges to database";
        auto& shaMapStore =
            dynamic_cast<SHAMapStoreImp&>(context.app.getSHAMapStore());
        shaMapStore.savePinnedRanges(
            context.app.getLedgerMaster().getPinnedLedgersRangeSet());
    }

    JLOG(j.info()) << "Catalogue load complete";

    // Now that all ledgers are saved and job queue is stopped, advance to the
    // latest one
    if (prevLedger && prevLedger->info().seq == header.max_ledger)
    {
        // CRITICAL: Insert the last ledger into mLedgerHistory for publishing.
        // During bulk loading, we avoided inserting ledgers into history to
        // prevent memory bloat. But switchLCL -> tryAdvance ->
        // findNewLedgersToPublish needs to find the ledger in history to
        // publish it properly. We use pin=false here to force insertion into
        // mLedgerHistory. The modified findNewLedgersToPublish will skip
        // publishing other pinned ledgers (they're already saved), but will
        // always publish the most recent.
        context.app.getLedgerMaster().storeLedger(prevLedger, false);

        JLOG(j.info()) << "Setting current ledger to seq "
                       << prevLedger->info().seq;
        context.app.getLedgerMaster().switchLCL(prevLedger);
    }

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

    return jvResult;
}

}  // namespace ripple