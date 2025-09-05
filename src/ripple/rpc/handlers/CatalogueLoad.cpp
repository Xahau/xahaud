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
 * catalogue_load: A Necessary Compromise
 *
 * WHY THIS EXISTS: While custom tools are superior for offline bulk loading,
 * catalogue_load is the ONLY option for expanding a live server's ledger range
 * without downtime. Neither SQLite nor NuDB support multi-process writes.
 *
 * REALITY CHECK: We're fighting against:
 * - SQLite write locks (online_delete pruning + per-ledger inserts)
 * - WAL checkpointing causing periodic stalls
 * - SHAMap's complex COW implementation
 * - Competition with P2P sync and RPC serving
 *
 * 3-QUEUE ARCHITECTURE WITH JobQueueAdapter:
 *
 * Main Thread (RPC handler thread):
 *   - Deserialize ledgers from catalogue file
 *   - Canonicalize state maps (unshare() for thread safety)
 *   - MUST be serial due to COW dependencies between ledgers
 *   - Feeds work to → Flush Queue
 *
 * Flush Queue (via JobQueueAdapter):
 *   - Flush canonicalized nodes to disk (NuDB/RocksDB)
 *   - Verify hashes after flushing
 *   - Call setImmutable() to finalize ledger
 *   - Can parallelize (nodes are immutable after canonicalization)
 *   - Uses CatalogueJobType::FLUSH_SAVE
 *   - Feeds completed ledgers to → SQL Queue
 *
 * SQL Queue (via JobQueueAdapter):
 *   - Save ledger metadata to SQLite
 *   - Single-threaded to avoid lock contention
 *   - Batch operations where possible for efficiency
 *   - Uses CatalogueJobType::SQL_SAVE
 *
 * JobQueueAdapter ROUTING:
 * The JobQueueAdapter intelligently routes jobs based on environment:
 * - Standalone mode: Creates its own JobQueue with aggressive thread counts
 *   and high priorities for maximum throughput
 * - Production mode: Routes through app's JobQueue with polite priorities
 *   (FLUSH_SAVE → jtPUBOLDLEDGER, SQL_SAVE → jtGENERIC) to play nicely
 *   with existing server workload
 *
 * NATURAL SPACING BENEFITS:
 * The queue separation creates breathing room for the production server:
 * - P2P sync can process incoming ledgers between operations
 * - RPC handlers get CPU time without starvation
 * - SQLite WAL checkpoints happen at natural boundaries
 * - Memory pressure stays manageable
 * - JobQueueAdapter ensures proper priority scheduling in production
 *
 * EXPECTATION: This provides "good enough" performance while staying polite.
 * Think "background expansion" not "bulk import". For new node setup, use
 * custom offline tools.
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
        std::atomic<uint64_t> totalJobs{0};
        std::atomic<uint64_t> totalMicroseconds{0};
        std::atomic<uint64_t> lastIntervalJobs{0};
        std::atomic<uint64_t> lastIntervalMicroseconds{0};
        std::atomic<uint64_t> mainThreadJobs{
            0};  // Jobs executed in main thread
        std::atomic<uint64_t> backgroundJobs{0};  // Jobs executed in background
        std::atomic<uint64_t> flushMainThread{0};  // Flush jobs in main thread
        std::atomic<uint64_t> flushBackground{0};  // Flush jobs in background
        std::chrono::steady_clock::time_point startTime;

        SQLMetrics() : startTime(std::chrono::steady_clock::now())
        {
        }
    };
    auto sqlMetrics = std::make_shared<SQLMetrics>();

    // Create JobQueueAdapter for intelligent job routing
    JobQueueAdapter jobAdapter(
        context.app,
        context.app.logs().journal("CatalogueJobs"),
        context.app.config()
            .standalone());  // Force standalone mode if configured

    // THREE-STAGE PIPELINE WITH WORK-STEALING:
    //
    // Stage 1 - Main Thread:
    //   - Deserialize + canonicalize (must be serial for COW)
    //   - addOrRunJob(FLUSH_SAVE) → queues or executes flush
    //
    // Stage 2 - Flush Job:
    //   - Flush nodes to disk (NuDB/RocksDB)
    //   - Verify hashes
    //   - setImmutable()
    //   - addOrRunJob(SQL_SAVE) → queues or executes SQL
    //
    // Stage 3 - SQL Job (terminal):
    //   - Save ledger metadata to SQLite
    //   - Mark complete in SaveState
    //
    // Work-Stealing Benefits:
    // - Maximum parallelism: 3 ledgers in flight (main, flush, SQL)
    // - Natural backpressure: each stage helps next if queue full
    // - No idle waiting: threads always working or helping
    // - Simple dependencies: each job knows its next step
    //
    // The addOrRunJob() pattern means any thread can help at any stage,
    // preventing queue overflow while maintaining pipeline efficiency.

    // Shared state for concurrent save management
    struct SaveState
    {
        std::atomic<int> pendingSaves{0};
        std::atomic<int> totalPendingJobs{0};
        std::mutex completionMutex;
        std::condition_variable completionCV;
        RangeSet<uint32_t> completedSaves;
        std::mutex completedSavesMutex;
        std::atomic<bool> hasError{false};
        std::string errorMessage;
        std::mutex errorMutex;

        // SQL serialization - only one SQL job at a time
        std::mutex sqlMutex;
        std::atomic<int> pendingSQLJobs{0};
    };

    // REMOVED: State map flush synchronization is no longer needed!
    // With the canonicalize-then-flush approach, nodes are immutable (cowid=0)
    // and safe for concurrent access. Multiple background threads can flush
    // different ledgers simultaneously without any race conditions.
    //
    // The canonicalization via unshare() in the main thread:
    // 1. Computes all hashes
    // 2. Marks nodes clean (cowid=0 - immutable)
    // 3. Makes nodes thread-safe for concurrent reads
    //
    // This allows parallel flushing without synchronization overhead.

    // REMOVED: FlushChainLink no longer needed with canonicalized nodes
    // Canonicalized nodes are immutable and safe for concurrent access

    // Structures for parallel save processing
    struct LedgerSaveJob
    {
        std::shared_ptr<Ledger> ledger;  // The ledger to save and flush
        std::shared_ptr<Ledger>
            parentLedger;      // Parent for delta flushing (null for first)
        bool flushMapsInMain;  // Whether maps were already flushed in main
                               // thread
        uint256 expectedHash;  // Expected hash to verify after flushing
        LedgerInfo info;       // The ledger info from the catalogue file

        void
        execute(
            Application& app,
            beast::Journal journal,
            std::shared_ptr<SaveState> saveState,
            JobQueueAdapter& jobAdapter,
            std::shared_ptr<SQLMetrics> sqlMetrics,
            bool isMainThread = false)  // Track if executing in main thread
        {
            auto j = journal;

            // Track thread distribution for flush jobs
            if (isMainThread)
                sqlMetrics->flushMainThread++;
            else
                sqlMetrics->flushBackground++;

            JLOG(j.trace())
                << "Executing save job for ledger " << ledger->info().seq
                << " hash: " << ledger->info().hash;

            //@@start catalogue-shamaps-flush-canonicalized
            // Flush nodes using pointer diff for efficiency
            int stateNodesFlushed = 0;
            int txNodesFlushed = 0;

            JLOG(j.trace()) << "Flushing ledger " << ledger->info().seq
                            << " (state canonical, tx dirty)";

            if (!flushMapsInMain)
            {
                // With canonicalization, nodes have cowid=0 (immutable)
                // We can use pointer diff safely!

                // Use pointer diff for all ledgers (handles first ledger
                // specially)
                JLOG(j.trace())
                    << "Using flushByPointerDiff for ledger "
                    << ledger->info().seq
                    << (parentLedger ? " (with parent)" : " (first ledger)");

                // flushByPointerDiff handles the first ledger case internally:
                // - If parent is provided: does efficient pointer diff
                // - If no parent (first ledger): walks and flushes entire tree
                stateNodesFlushed = ledger->stateMap().flushByPointerDiff(
                    parentLedger
                        ? std::optional<std::reference_wrapper<const SHAMap>>(
                              parentLedger->stateMap())
                        : std::nullopt,
                    pinnedACCOUNT_NODE);

                JLOG(j.trace()) << "State map flushed " << stateNodesFlushed
                                << " nodes for ledger " << ledger->info().seq;

                // TX map - use flushDirty since TX maps are unique per ledger
                // No COW sharing between ledgers for TX maps, so it's safe
                txNodesFlushed =
                    ledger->txMap().flushDirty(pinnedTRANSACTION_NODE);

                JLOG(j.trace()) << "TX map flushed " << txNodesFlushed
                                << " nodes for ledger " << ledger->info().seq;

                // NOW verify TX hash AFTER flushing (this will canonicalize it)
                auto computedTxHash = ledger->txMap().getHash().as_uint256();
                if (computedTxHash != info.txHash)
                {
                    JLOG(j.error())
                        << "TX hash mismatch for ledger " << ledger->info().seq
                        << " Expected: " << info.txHash
                        << " Got: " << computedTxHash;

                    // Set error flag so main thread knows to abort
                    saveState->hasError = true;
                    {
                        std::lock_guard<std::mutex> lock(saveState->errorMutex);
                        saveState->errorMessage =
                            "TX hash mismatch for ledger " +
                            std::to_string(ledger->info().seq);
                    }
                    return;
                }

                // No need for synchronization - canonical nodes are immutable!
                // Multiple ledgers can flush in parallel safely
            }

            // Log periodically for debugging
            if (ledger->info().seq % 1000 == 0)
            {
                JLOG(j.trace())
                    << "BG flush ledger " << ledger->info().seq << ": "
                    << stateNodesFlushed << " state nodes, " << txNodesFlushed
                    << " tx nodes "
                    << (flushMapsInMain ? "(already flushed in main)"
                                        : (parentLedger ? "(delta flush)"
                                                        : "(full flush)"));
            }
            //@@end catalogue-shamaps-flush-dirty

            // Finalize the ledger (moved from main thread to avoid getHash
            // calls) Use the info from the catalogue file, not ledger->info()
            ledger->setAccepted(
                info.closeTime,
                info.closeTimeResolution,
                info.closeFlags & sLCF_NoConsensusTime);
            ledger->setValidated();
            ledger->setCloseFlags(info.closeFlags);

            // NOW make the ledger immutable AFTER flushing
            // This sets important header fields needed for SQLite save
            ledger->setImmutable(true);

            // Verify the hash matches what was expected
            if (ledger->info().hash != expectedHash)
            {
                JLOG(j.error())
                    << "Ledger seq=" << ledger->info().seq
                    << " hash mismatch after flush! Expected: " << expectedHash
                    << " Got: " << ledger->info().hash;

                // Set error flag so main thread knows to abort
                saveState->hasError = true;
                {
                    std::lock_guard<std::mutex> lock(saveState->errorMutex);
                    saveState->errorMessage =
                        "Catalogue file contains a corrupted ledger at "
                        "sequence " +
                        std::to_string(ledger->info().seq);
                }
                return;
            }

            auto seq = ledger->info().seq;

            // SQL save logic - run inline for now to avoid contention
            // TODO: Implement addJobOrRunOrSkip() for better control
            {
                JLOG(j.trace()) << "Executing SQL inline for ledger " << seq;

                // Track metrics
                if (isMainThread)
                    sqlMetrics->mainThreadJobs++;
                else
                    sqlMetrics->backgroundJobs++;

                // Time the SQL job execution
                auto start = std::chrono::steady_clock::now();

                // Get the database
                auto const db =
                    dynamic_cast<SQLiteDatabase*>(&app.getRelationalDatabase());
                if (!db)
                {
                    JLOG(j.error())
                        << "Failed to get database for ledger " << seq;
                    saveState->hasError = true;
                    return;
                }

                // This handles the existence check and calls
                // detail::saveValidatedLedger
                bool success = db->saveValidatedLedger(ledger, false);

                auto end = std::chrono::steady_clock::now();
                auto duration =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        end - start)
                        .count();

                sqlMetrics->totalJobs++;
                sqlMetrics->totalMicroseconds += duration;
                sqlMetrics->lastIntervalJobs++;
                sqlMetrics->lastIntervalMicroseconds += duration;

                if (!success)
                {
                    JLOG(j.error())
                        << "Failed to save ledger " << seq << " to SQLite";
                    // Set error flag so main thread knows what happened
                    saveState->hasError = true;
                    {
                        std::lock_guard<std::mutex> lock(saveState->errorMutex);
                        saveState->errorMessage = "Failed to save ledger " +
                            std::to_string(seq) + " to SQLite database";
                    }
                }
                else
                {
                    JLOG(j.trace()) << "SQL: Successfully saved ledger " << seq;
                }

                // Mark this ledger as saved for range tracking
                {
                    std::lock_guard lock(saveState->completedSavesMutex);
                    saveState->completedSaves.insert(seq);
                }
            }

            JLOG(j.trace()) << "Save job completed for ledger " << seq
                            << " stateNodes: " << stateNodesFlushed
                            << " txNodes: " << txNodesFlushed;
        }
    };

    auto saveState = std::make_shared<SaveState>();

    static constexpr int MAX_CONCURRENT_SAVES =
        10;  // Allow even more parallel saves
    static constexpr int BATCH_UPDATE_INTERVAL =
        100;  // Update ranges every N ledgers

    // IMPORTANT: We use the canonicalize-then-flush approach!
    //
    // The safe pattern is:
    // 1. Build the ledger in main thread
    // 2. Canonicalize in main thread (walkSubTree with doWrite=false)
    //    - This computes all hashes and marks nodes clean (cowid=0)
    //    - Makes nodes immutable and safe for concurrent access
    // 3. Pass canonicalized ledger to background thread
    // 4. Background thread uses flushByPointerDiff for efficient I/O
    //    - Safe because nodes are immutable after canonicalization
    //    - Pointer diff works because canonical nodes have stable pointers
    // 5. Background thread calls setImmutable() to finalize
    //
    // This separates CPU work (hashing) in main from I/O work (flushing) in
    // background
    static constexpr bool flushMapsInMain = false;
    static constexpr bool canonicalizeInMain =
        true;  // NEW: Canonicalize for thread safety
    static constexpr bool separateSQLJob =
        false;  // EXPERIMENT: false = SQL runs inline in flush job (serialized)
                //             true = SQL runs as separate job (can be parallel)

    uint32_t ledgersLoaded = 0;
    std::shared_ptr<Ledger> prevLedger;
    uint32_t expected_seq = header.min_ledger;

    // No synchronization chain needed with canonicalized nodes

    // Process ledgers with parallel flushing
    //
    // BREAKTHROUGH: With canonicalize-then-flush, parallel processing WORKS!
    //
    // The key insight: canonicalization via unshare() makes nodes immutable
    // (cowid=0), which enables safe concurrent access from multiple threads.
    //
    // The safe pattern is:
    // 1. Build ledger in main thread (sequential for COW safety)
    // 2. Canonicalize state map via unshare() (makes nodes immutable)
    // 3. Pass to background thread for parallel flushing
    // 4. Multiple ledgers can flush simultaneously (no races!)
    //
    // Why this works:
    // - unshare() calls walkSubTree(false, hotUNKNOWN) internally
    // - This computes all hashes and marks nodes clean (cowid=0)
    // - Nodes with cowid=0 are immutable and thread-safe
    // - flushByPointerDiff efficiently writes immutable nodes
    //
    // TX maps don't need canonicalization because:
    // - Each ledger has its own unique TX map (no COW sharing)
    // - No shared nodes between ledgers = no race conditions
    // - Can use regular flushDirty() safely in background
    //
    // Performance benefits:
    // - CPU work (hashing) separated from I/O work (flushing)
    // - Multiple ledgers flush in parallel without synchronization
    // - No lock contention or cache line bouncing
    // - Dramatic speedup on multi-core systems
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
        if (canonicalizeInMain)
        {
            // unshare() calls walkSubTree(false, hotUNKNOWN) internally
            // This computes hashes and marks nodes clean (cowid=0)
            // Making nodes immutable and safe for background access
            JLOG(j.trace()) << "Canonicalizing state map for ledger "
                            << info.seq << " in main thread...";

            // Canonicalize state map ONLY (computes hashes, marks clean)
            // This is needed for thread safety due to COW sharing between
            // ledgers
            ledger->stateMap().unshare();

            // DON'T canonicalize TX map - let background flush it normally
            // TX maps are unique per ledger (no COW sharing), so they're safe
            // to flush in background without canonicalization

            // Only verify STATE hash here (after canonicalization)
            auto computedStateHash = ledger->stateMap().getHash().as_uint256();
            if (computedStateHash != info.accountHash)
            {
                JLOG(j.error()) << "State hash mismatch for ledger " << info.seq
                                << " Expected: " << info.accountHash
                                << " Got: " << computedStateHash;
                return rpcError(
                    rpcINTERNAL, "State hash mismatch after canonicalization");
            }

            // TX hash will be verified in background BEFORE flushing
            // (calling getHash() here would canonicalize and prevent flushing)

            JLOG(j.trace()) << "Ledger " << info.seq
                            << " state map canonicalized and verified";
        }

        // Queue save job for parallel processing using our temporary JobQueue
        {
            // Ledger is now canonicalized - safe for background processing

            // No flush chain needed with canonicalized nodes

            // Wait if we're at the concurrent save limit
            {
                std::unique_lock<std::mutex> lock(saveState->completionMutex);
                if (saveState->pendingSaves >= MAX_CONCURRENT_SAVES)
                {
                    JLOG(j.trace()) << "Waiting for save slots, pending="
                                    << saveState->pendingSaves.load();
                    saveState->completionCV.wait(lock, [&saveState]() {
                        return saveState->pendingSaves < MAX_CONCURRENT_SAVES;
                    });
                }
            }

            // Queue the bundled save job
            saveState->pendingSaves++;
            saveState->totalPendingJobs++;

            JLOG(j.trace()) << "Queueing save job for ledger " << ledger->seq()
                            << " hash: " << ledger->info().hash
                            << ", pending=" << saveState->pendingSaves.load();

            // Use addOrRunJob to queue flush job or run immediately if queue
            // full Since FLUSH_SAVE is limited to 2 concurrent, we won't
            // overwhelm SQLite
            {
                LedgerSaveJob job{
                    ledger,
                    prevLedger,  // Pass parent ledger for delta flushing
                    flushMapsInMain,
                    info.hash,  // Pass the expected hash from the catalogue
                    info};      // Pass the complete LedgerInfo from catalogue

                // Capture main thread ID for comparison
                auto mainThreadId = std::this_thread::get_id();

                // Queue or execute the flush job using work-stealing pattern
                // With only 1 flush job max, SQL won't get overwhelmed
                bool executedInline = jobAdapter.addOrRunJob(
                    CatalogueJobType::FLUSH_SAVE,
                    "flush-ledger-" + std::to_string(ledger->seq()),
                    [job,
                     &context,
                     j,
                     saveState,
                     &jobAdapter,
                     sqlMetrics,
                     mainThreadId]() mutable {
                        // Check if we're running in the main thread
                        bool inMainThread =
                            (std::this_thread::get_id() == mainThreadId);
                        job.execute(
                            context.app,
                            j,
                            saveState,
                            jobAdapter,
                            sqlMetrics,
                            inMainThread);
                        saveState->pendingSaves--;
                        saveState->totalPendingJobs--;
                        saveState->completionCV.notify_all();
                    });
            }

            // No chain update needed - parallel flushing is safe
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
            auto sqlJobsInterval = sqlMetrics->lastIntervalJobs.exchange(0);
            auto sqlMicrosInterval =
                sqlMetrics->lastIntervalMicroseconds.exchange(0);
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
            auto totalSQLJobs = sqlMetrics->totalJobs.load();
            auto totalSQLMicros = sqlMetrics->totalMicroseconds.load();
            if (totalSQLJobs > 0)
            {
                perfJson["sql_total_jobs"] =
                    static_cast<Json::UInt>(totalSQLJobs);
                perfJson["sql_avg_ms_overall"] =
                    std::round(totalSQLMicros / 1000.0 / totalSQLJobs * 10) /
                    10;
            }

            // SQL thread execution metrics
            auto mainJobs = sqlMetrics->mainThreadJobs.load();
            auto bgJobs = sqlMetrics->backgroundJobs.load();
            if (mainJobs > 0 || bgJobs > 0)
            {
                perfJson["sql_main_thread"] = static_cast<Json::UInt>(mainJobs);
                perfJson["sql_background"] = static_cast<Json::UInt>(bgJobs);
                if (mainJobs + bgJobs > 0)
                {
                    perfJson["sql_main_percent"] =
                        std::round(
                            mainJobs * 100.0 / (mainJobs + bgJobs) * 10) /
                        10;
                }
            }

            // Flush thread execution metrics
            auto flushMain = sqlMetrics->flushMainThread.load();
            auto flushBg = sqlMetrics->flushBackground.load();
            if (flushMain > 0 || flushBg > 0)
            {
                perfJson["flush_main_thread"] =
                    static_cast<Json::UInt>(flushMain);
                perfJson["flush_background"] = static_cast<Json::UInt>(flushBg);
                if (flushMain + flushBg > 0)
                {
                    perfJson["flush_main_percent"] =
                        std::round(
                            flushMain * 100.0 / (flushMain + flushBg) * 10) /
                        10;
                }
            }

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

        // Periodically update ranges and sweep cache
        if (ledgersLoaded % BATCH_UPDATE_INTERVAL == 0)
        {
            // Wait for pending saves to complete
            {
                std::unique_lock<std::mutex> lock(saveState->completionMutex);
                saveState->completionCV.wait(lock, [&saveState]() {
                    return saveState->pendingSaves == 0;
                });
            }

            // Check for errors from background jobs
            if (saveState->hasError)
            {
                std::lock_guard<std::mutex> lock(saveState->errorMutex);
                JLOG(j.error())
                    << "Background job error: " << saveState->errorMessage;
                return rpcError(rpcINTERNAL, saveState->errorMessage);
            }

            // Update ledger ranges for all completed saves
            {
                std::lock_guard lock(saveState->completedSavesMutex);
                if (!saveState->completedSaves.empty())
                {
                    // Get all ranges that have been saved
                    // RangeSet uses interval_set, iterate through intervals
                    for (auto const& interval : saveState->completedSaves)
                    {
                        auto first = interval.lower();
                        auto last = interval.upper();
                        context.app.getLedgerMaster().setLedgerRangePresent(
                            first, last, do_pinning);
                        JLOG(j.info())
                            << "Updated ledger range: " << first << "-" << last;
                    }
                    // Clear for next batch
                    saveState->completedSaves.clear();
                }
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

    decompStream->reset();
    infile.close();

    // Wait for all pending save jobs to complete
    if (saveState->totalPendingJobs > 0)
    {
        JLOG(j.info()) << "Waiting for " << saveState->totalPendingJobs.load()
                       << " pending save jobs to complete...";

        std::unique_lock<std::mutex> lock(saveState->completionMutex);
        saveState->completionCV.wait(
            lock, [&saveState]() { return saveState->totalPendingJobs == 0; });

        JLOG(j.info()) << "All save jobs completed";
    }

    // Final error check after all jobs are done
    if (saveState->hasError)
    {
        std::lock_guard<std::mutex> lock(saveState->errorMutex);
        JLOG(j.error()) << "Background job error: " << saveState->errorMessage;
        return rpcError(rpcINTERNAL, saveState->errorMessage);
    }

    // Update ledger ranges for any remaining completed saves
    {
        std::lock_guard lock2(saveState->completedSavesMutex);
        if (!saveState->completedSaves.empty())
        {
            for (auto const& interval : saveState->completedSaves)
            {
                auto first = interval.lower();
                auto last = interval.upper();
                context.app.getLedgerMaster().setLedgerRangePresent(
                    first, last, do_pinning);
                JLOG(j.info())
                    << "Final update ledger range: " << first << "-" << last;
            }
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

    // Stop the JobQueueAdapter (waits for all jobs to complete)
    jobAdapter.stop();
    JLOG(j.info()) << "Catalogue load processing complete";

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