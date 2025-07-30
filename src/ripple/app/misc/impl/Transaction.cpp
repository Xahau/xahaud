//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/app/rdb/backend/PostgresDatabase.h>
#include <ripple/app/rdb/backend/SQLiteDatabase.h>
#include <ripple/app/tx/apply.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/safe_cast.h>
#include <ripple/core/DatabaseCon.h>
#include <ripple/core/Pg.h>
#include <ripple/json/json_reader.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/CTID.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <thread>
#include <unordered_map>
#include <vector>

#define ENABLE_PERFORMANCE_TRACKING 0

namespace ripple {

#if ENABLE_PERFORMANCE_TRACKING

// Performance monitoring statistics
namespace {
// Design: Uses thread-local storage for most stats to avoid contention.
// Only global concurrency tracking uses atomics, as it requires cross-thread
// visibility. Statistics are aggregated using dirty reads for minimal
// performance impact.

// Thread-local statistics - no synchronization needed!
struct ThreadLocalStats
{
    uint64_t executionCount = 0;
    uint64_t totalTimeNanos = 0;
    uint64_t totalKeys = 0;
    uint32_t currentlyExecuting = 0;  // 0 or 1 for this thread
    std::thread::id threadId = std::this_thread::get_id();

    // For global registry
    ThreadLocalStats* next = nullptr;

    ThreadLocalStats();
    ~ThreadLocalStats();
};

// Global registry of thread-local stats (only modified during thread
// creation/destruction)
struct GlobalRegistry
{
    std::atomic<ThreadLocalStats*> head{nullptr};
    std::atomic<uint64_t> globalExecutions{0};
    std::atomic<uint32_t> globalConcurrent{
        0};  // Current global concurrent executions
    std::atomic<uint32_t> maxGlobalConcurrent{0};  // Max observed

    // For tracking concurrency samples
    std::vector<uint32_t> concurrencySamples;
    std::mutex sampleMutex;  // Only used during printing

    std::chrono::steady_clock::time_point startTime =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastPrintTime =
        std::chrono::steady_clock::now();

    static constexpr auto PRINT_INTERVAL = std::chrono::seconds(10);
    static constexpr uint64_t PRINT_EVERY_N_CALLS = 1000;

    void
    registerThread(ThreadLocalStats* stats)
    {
        // Add to linked list atomically
        ThreadLocalStats* oldHead = head.load();
        do
        {
            stats->next = oldHead;
        } while (!head.compare_exchange_weak(oldHead, stats));
    }

    void
    unregisterThread(ThreadLocalStats* stats)
    {
        // In production, you'd want proper removal logic
        // For this example, we'll just leave it in the list
        // (threads typically live for the process lifetime anyway)
    }

    void
    checkAndPrint(uint64_t localCount)
    {
        // Update approximate global count
        uint64_t approxGlobal =
            globalExecutions.fetch_add(localCount) + localCount;

        auto now = std::chrono::steady_clock::now();
        if (approxGlobal % PRINT_EVERY_N_CALLS < localCount ||
            (now - lastPrintTime) >= PRINT_INTERVAL)
        {
            // Only one thread prints at a time
            static std::atomic<bool> printing{false};
            bool expected = false;
            if (printing.compare_exchange_strong(expected, true))
            {
                // Double-check timing
                now = std::chrono::steady_clock::now();
                if ((now - lastPrintTime) >= PRINT_INTERVAL)
                {
                    printStats();
                    lastPrintTime = now;
                }
                printing = false;
            }
        }
    }

    void
    printStats()
    {
        // Dirty read of all thread-local stats
        uint64_t totalExecs = 0;
        uint64_t totalNanos = 0;
        uint64_t totalKeyCount = 0;
        uint32_t currentConcurrent = globalConcurrent.load();
        uint32_t maxConcurrent = maxGlobalConcurrent.load();
        std::unordered_map<
            std::thread::id,
            std::tuple<uint64_t, uint64_t, uint64_t>>
            threadData;

        // Walk the linked list of thread-local stats
        ThreadLocalStats* current = head.load();
        while (current)
        {
            // Dirty reads - no synchronization!
            uint64_t execs = current->executionCount;
            if (execs > 0)
            {
                uint64_t nanos = current->totalTimeNanos;
                uint64_t keys = current->totalKeys;

                totalExecs += execs;
                totalNanos += nanos;
                totalKeyCount += keys;

                threadData[current->threadId] = {execs, nanos, keys};
            }
            current = current->next;
        }

        if (totalExecs == 0)
            return;

        double avgTimeUs =
            static_cast<double>(totalNanos) / totalExecs / 1000.0;
        double avgKeys = static_cast<double>(totalKeyCount) / totalExecs;
        double totalTimeMs = static_cast<double>(totalNanos) / 1000000.0;

        // Calculate wall clock time elapsed
        auto now = std::chrono::steady_clock::now();
        auto wallTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - startTime)
                              .count();
        double effectiveParallelism = wallTimeMs > 0
            ? totalTimeMs / static_cast<double>(wallTimeMs)
            : 0.0;

        std::cout
            << "\n=== Transaction::tryDirectApply Performance Stats ===\n";
        std::cout << "Total executions: ~" << totalExecs << " (dirty read)\n";
        std::cout << "Wall clock time: " << wallTimeMs << " ms\n";
        std::cout << "Total CPU time: " << std::fixed << std::setprecision(2)
                  << totalTimeMs << " ms\n";
        std::cout << "Effective parallelism: " << std::fixed
                  << std::setprecision(2) << effectiveParallelism << "x\n";
        std::cout << "Average time: " << std::fixed << std::setprecision(2)
                  << avgTimeUs << " μs\n";
        std::cout << "Average keys touched: " << std::fixed
                  << std::setprecision(2) << avgKeys << "\n";
        std::cout << "Current concurrent executions: " << currentConcurrent
                  << "\n";
        std::cout << "Max concurrent observed: " << maxConcurrent << "\n";
        std::cout << "Active threads: " << threadData.size() << "\n";

        std::cout << "Thread distribution:\n";

        // Sort threads by total time spent (descending)
        std::vector<std::pair<
            std::thread::id,
            std::tuple<uint64_t, uint64_t, uint64_t>>>
            sortedThreads(threadData.begin(), threadData.end());
        std::sort(
            sortedThreads.begin(),
            sortedThreads.end(),
            [](const auto& a, const auto& b) {
                return std::get<1>(a.second) >
                    std::get<1>(b.second);  // Sort by time
            });

        for (const auto& [tid, data] : sortedThreads)
        {
            auto [count, time, keys] = data;
            double percentage =
                (static_cast<double>(count) / totalExecs) * 100.0;
            double avgThreadTimeUs = static_cast<double>(time) / count / 1000.0;
            double totalThreadTimeMs = static_cast<double>(time) / 1000000.0;
            double timePercentage =
                (static_cast<double>(time) / totalNanos) * 100.0;
            std::cout << "  Thread " << tid << ": " << count << " executions ("
                      << std::fixed << std::setprecision(1) << percentage
                      << "%), total " << std::setprecision(2)
                      << totalThreadTimeMs << " ms (" << std::setprecision(1)
                      << timePercentage << "% of time), avg "
                      << std::setprecision(2) << avgThreadTimeUs << " μs\n";
        }

        std::cout << "Hardware concurrency: "
                  << std::thread::hardware_concurrency() << "\n";
        std::cout << "===================================================\n\n";
        std::cout.flush();
    }
};

static GlobalRegistry globalRegistry;

// Thread-local instance
thread_local ThreadLocalStats tlStats;

// Constructor/destructor for thread registration
ThreadLocalStats::ThreadLocalStats()
{
    globalRegistry.registerThread(this);
}

ThreadLocalStats::~ThreadLocalStats()
{
    globalRegistry.unregisterThread(this);
}

// RAII class to track concurrent executions (global)
class ConcurrentExecutionTracker
{
    // Note: This introduces minimal atomic contention to track true global
    // concurrency. The alternative would miss concurrent executions between
    // print intervals.
public:
    ConcurrentExecutionTracker()
    {
        tlStats.currentlyExecuting = 1;

        // Update global concurrent count
        uint32_t current = globalRegistry.globalConcurrent.fetch_add(1) + 1;

        // Update max if needed (only contends when setting new maximum)
        uint32_t currentMax = globalRegistry.maxGlobalConcurrent.load();
        while (current > currentMax &&
               !globalRegistry.maxGlobalConcurrent.compare_exchange_weak(
                   currentMax, current))
        {
            // Loop until we successfully update or current is no longer >
            // currentMax
        }
    }

    ~ConcurrentExecutionTracker()
    {
        tlStats.currentlyExecuting = 0;
        globalRegistry.globalConcurrent.fetch_sub(1);
    }
};
}  // namespace

#endif  // ENABLE_PERFORMANCE_TRACKING

Transaction::Transaction(
    std::shared_ptr<STTx const> const& stx,
    std::string& reason,
    Application& app) noexcept
    : mTransaction(stx), mApp(app), j_(app.journal("Ledger"))
{
    try
    {
        mTransactionID = mTransaction->getTransactionID();

        OpenView sandbox(*app.openLedger().current());

        sandbox.getAndResetKeysTouched();

        ApplyFlags flags{0};

#if ENABLE_PERFORMANCE_TRACKING
        ConcurrentExecutionTracker concurrentTracker;
        auto startTime = std::chrono::steady_clock::now();
#endif

        if (auto directApplied =
                app.getTxQ().tryDirectApply(app, sandbox, stx, flags, j_))
            keysTouched = sandbox.getAndResetKeysTouched();

#if ENABLE_PERFORMANCE_TRACKING
        auto endTime = std::chrono::steady_clock::now();
        auto elapsedNanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                endTime - startTime)
                .count();

        tlStats.executionCount++;
        tlStats.totalTimeNanos += elapsedNanos;
        tlStats.totalKeys += keysTouched.size();

        if (tlStats.executionCount % 100 == 0)
        {
            globalRegistry.checkAndPrint(100);
        }
#endif
    }
    catch (std::exception& e)
    {
        reason = e.what();
        return;
    }

    mStatus = NEW;
}

//
// Misc.
//

void
Transaction::setStatus(
    TransStatus ts,
    std::uint32_t lseq,
    std::optional<std::uint32_t> tseq,
    std::optional<std::uint16_t> netID)
{
    mStatus = ts;
    mInLedger = lseq;
    if (tseq)
        mTxnSeq = tseq;
    if (netID)
        mNetworkID = netID;
}

TransStatus
Transaction::sqlTransactionStatus(boost::optional<std::string> const& status)
{
    char const c = (status) ? (*status)[0] : safe_cast<char>(txnSqlUnknown);

    switch (c)
    {
        case txnSqlNew:
            return NEW;
        case txnSqlConflict:
            return CONFLICTED;
        case txnSqlHeld:
            return HELD;
        case txnSqlValidated:
            return COMMITTED;
        case txnSqlIncluded:
            return INCLUDED;
    }

    assert(c == txnSqlUnknown);
    return INVALID;
}

Transaction::pointer
Transaction::transactionFromSQL(
    boost::optional<std::uint64_t> const& ledgerSeq,
    boost::optional<std::string> const& status,
    Blob const& rawTxn,
    Application& app)
{
    std::uint32_t const inLedger =
        rangeCheckedCast<std::uint32_t>(ledgerSeq.value_or(0));

    SerialIter it(makeSlice(rawTxn));
    auto txn = std::make_shared<STTx const>(it);
    std::string reason;
    auto tr = std::make_shared<Transaction>(txn, reason, app);

    tr->setStatus(sqlTransactionStatus(status));
    tr->setLedger(inLedger);
    return tr;
}

std::variant<
    std::pair<std::shared_ptr<Transaction>, std::shared_ptr<TxMeta>>,
    TxSearched>
Transaction::load(uint256 const& id, Application& app, error_code_i& ec)
{
    return load(id, app, std::nullopt, ec);
}

std::variant<
    std::pair<std::shared_ptr<Transaction>, std::shared_ptr<TxMeta>>,
    TxSearched>
Transaction::load(
    uint256 const& id,
    Application& app,
    ClosedInterval<uint32_t> const& range,
    error_code_i& ec)
{
    using op = std::optional<ClosedInterval<uint32_t>>;

    return load(id, app, op{range}, ec);
}

Transaction::Locator
Transaction::locate(uint256 const& id, Application& app)
{
    auto const db =
        dynamic_cast<PostgresDatabase*>(&app.getRelationalDatabase());

    if (!db)
    {
        Throw<std::runtime_error>("Failed to get relational database");
    }

    return db->locateTransaction(id);
}

std::variant<
    std::pair<std::shared_ptr<Transaction>, std::shared_ptr<TxMeta>>,
    TxSearched>
Transaction::load(
    uint256 const& id,
    Application& app,
    std::optional<ClosedInterval<uint32_t>> const& range,
    error_code_i& ec)
{
    auto const db = dynamic_cast<SQLiteDatabase*>(&app.getRelationalDatabase());

    if (!db)
    {
        Throw<std::runtime_error>("Failed to get relational database");
    }

    return db->getTransaction(id, range, ec);
}

// options 1 to include the date of the transaction
Json::Value
Transaction::getJson(JsonOptions options, bool binary) const
{
    Json::Value ret(mTransaction->getJson(JsonOptions::none, binary));

    if (mInLedger)
    {
        ret[jss::inLedger] = mInLedger;  // Deprecated.
        ret[jss::ledger_index] = mInLedger;

        if (options == JsonOptions::include_date)
        {
            auto ct = mApp.getLedgerMaster().getCloseTimeBySeq(mInLedger);
            if (ct)
                ret[jss::date] = ct->time_since_epoch().count();
        }

        // compute outgoing CTID
        // override local network id if it's explicitly in the txn
        std::optional netID = mNetworkID;
        if (mTransaction->isFieldPresent(sfNetworkID))
            netID = mTransaction->getFieldU32(sfNetworkID);

        if (mTxnSeq && netID && *mTxnSeq <= 0xFFFFU && *netID < 0xFFFFU &&
            mInLedger < 0xFFFFFFFUL)
        {
            std::optional<std::string> ctid =
                RPC::encodeCTID(mInLedger, *mTxnSeq, *netID);
            if (ctid)
                ret[jss::ctid] = *ctid;
        }
    }

    return ret;
}

}  // namespace ripple
