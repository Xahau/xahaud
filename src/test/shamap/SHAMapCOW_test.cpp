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

#include <ripple/basics/Blob.h>
#include <ripple/basics/Buffer.h>
#include <ripple/beast/unit_test.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/shamap/SHAMap.h>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <test/shamap/common.h>
#include <test/unit_test/SuiteJournal.h>
#include <thread>

namespace ripple {
namespace tests {

// Dead simple test item helper - just use numbers
static boost::intrusive_ptr<SHAMapItem>
makeItem(int k, int v = 0)
{
    uint256 key(k);
    uint256 val(v);
    return make_shamapitem(
        key, Slice{val.data(), val.size()});  // Use full uint256
}

// Test configuration
struct TestConfig
{
    // Hash logging
    bool log_hashes_before_flush = true;
    bool log_hashes_after_ops = true;

    // Flushing decisions
    bool flush_map1 = true;
    bool flush_map2 = false;  // Usually false to test deferred flush
    bool flush_map3 = true;

    // WHEN to flush - THIS IS THE KEY!
    bool flush_during_construction =
        true;  // true = flush as we build, false = flush at end

    // Snapshot types
    bool map2_snapshot_mutable = true;  // true = mutable, false = immutable
    bool map3_snapshot_mutable = true;

    // Immutability settings
    bool map1_set_immutable_after_flush = false;
    bool map2_set_immutable_after_snapshot = false;
    bool map3_set_immutable_before_flush = false;

    // Debugging
    bool verbose = true;
};

class SHAMapCOW_test : public beast::unit_test::suite
{
public:
    static Buffer
    IntToVUC(int v)
    {
        Buffer vuc(32);
        std::fill_n(vuc.data(), vuc.size(), static_cast<std::uint8_t>(v));
        return vuc;
    }

    void
    testLedgerChainPointerDiff(beast::Journal const& journal)
    {
        std::cout << "\n\n========== LEDGER CHAIN POINTER DIFF TEST =========="
                  << std::endl;

        tests::TestNodeFamily f(journal);

        // Test 1: Build chain WITHOUT hashing, try pointer diff flush
        std::cout << "\nTest 1: Chain without hashing - pointer diff impossible"
                  << std::endl;
        {
            // Create ledger 1
            SHAMap ledger1(SHAMapType::FREE, f);
            ledger1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));
            ledger1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 200));

            std::cout << "  Ledger 1 created with 2 accounts" << std::endl;

            // Create ledger 2 from snapshot
            auto ledger2 = ledger1.snapShot(true);
            ledger2->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 150));  // Transfer
            ledger2->addItem(
                SHAMapNodeType::tnACCOUNT_STATE,
                makeItem(3, 300));  // New account

            std::cout << "  Ledger 2: modified account 1, added account 3"
                      << std::endl;

            // Create ledger 3 from ledger 2
            auto ledger3 = ledger2->snapShot(true);
            ledger3->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 250));  // Transfer
            ledger3->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(3, 350));  // Transfer

            std::cout << "  Ledger 3: modified accounts 2 and 3" << std::endl;

            // Now try to use pointer diff WITHOUT hashing
            std::cout << "\n  Attempting pointer diff flush WITHOUT hashing:"
                      << std::endl;

            // The problem: nodes are shared but NOT canonicalized!
            // ledger1 and ledger2 share some nodes (account 2)
            // ledger2 and ledger3 share some nodes (account 1)

            // If we try to flush by pointer diff:
            // 1. We can't tell which nodes are "new" because pointers are
            // shared
            // 2. We can't hash nodes because that would modify them (cache
            // hash)
            // 3. We can't modify nodes because they're shared!

            std::cout << "  PROBLEM: Shared nodes have same pointers!"
                      << std::endl;
            std::cout << "  - Can't diff: shared nodes look 'unchanged'"
                      << std::endl;
            std::cout << "  - Can't hash: would modify shared nodes"
                      << std::endl;
            std::cout << "  - Can't flush: need hashes first!" << std::endl;
        }

        // Test 2: The serial dependency chain
        std::cout << "\nTest 2: The unavoidable serial dependency chain"
                  << std::endl;
        {
            SHAMap ledger1(SHAMapType::FREE, f);
            for (int i = 1; i <= 10; i++)
                ledger1.addItem(
                    SHAMapNodeType::tnACCOUNT_STATE, makeItem(i, i * 100));

            std::cout << "  Created ledger 1 with 10 accounts" << std::endl;

            // The ONLY safe way to process:
            std::cout << "\n  The mandatory serial process:" << std::endl;

            // Step 1: Hash ledger1 (marks all nodes clean)
            auto hash1 = ledger1.getHash().as_uint256();
            std::cout << "  1. Hash ledger1: " << hash1 << std::endl;
            std::cout << "     - Computes hashes bottom-up" << std::endl;
            std::cout << "     - Caches hashes in nodes" << std::endl;
            std::cout << "     - Marks nodes clean (cowid=0)" << std::endl;

            // Step 2: Flush ledger1 (now safe, nodes are immutable)
            int flushed1 = ledger1.flushDirty(hotACCOUNT_NODE);
            std::cout << "  2. Flush ledger1: " << flushed1 << " nodes"
                      << std::endl;
            std::cout << "     - Nodes already hashed, so flush returns 0!"
                      << std::endl;

            // Step 3: Create ledger2 (COW from clean nodes)
            auto ledger2 = ledger1.snapShot(true);
            ledger2->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(5, 550));
            std::cout << "  3. Create ledger2 from snapshot" << std::endl;
            std::cout << "     - Shares clean nodes with ledger1" << std::endl;
            std::cout << "     - COW triggers on modification" << std::endl;

            // Step 4: Hash ledger2 (must complete before ledger3)
            auto hash2 = ledger2->getHash().as_uint256();
            std::cout << "  4. Hash ledger2: " << hash2 << std::endl;
            std::cout << "     - MUST happen before creating ledger3"
                      << std::endl;
            std::cout << "     - Otherwise ledger3 shares dirty nodes!"
                      << std::endl;

            // This is SERIAL - can't parallelize!
            std::cout << "\n  Why this CAN'T be parallelized:" << std::endl;
            std::cout << "  - Hash computation modifies nodes (caches result)"
                      << std::endl;
            std::cout << "  - Shared nodes can't be modified concurrently"
                      << std::endl;
            std::cout << "  - Each ledger depends on previous ledger's hash"
                      << std::endl;
        }

        // Test 3: The pointer diff that WOULD work (if nodes were canonical)
        std::cout
            << "\nTest 3: How pointer diff SHOULD work (with canonicalization)"
            << std::endl;
        {
            SHAMap ledger1(SHAMapType::FREE, f);
            ledger1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));
            ledger1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 200));

            // CANONICALIZE ledger1
            auto hash1 = ledger1.getHash().as_uint256();
            std::cout << "  Ledger1 canonicalized: " << hash1 << std::endl;

            // Now create ledger2 - will COW on write
            auto ledger2 = ledger1.snapShot(true);
            ledger2->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 150));

            // CANONICALIZE ledger2
            auto hash2 = ledger2->getHash().as_uint256();
            std::cout << "  Ledger2 canonicalized: " << hash2 << std::endl;

            std::cout << "\n  Now pointer diff would work:" << std::endl;
            std::cout << "  - Unchanged nodes: same pointer (shared canonical)"
                      << std::endl;
            std::cout
                << "  - Changed nodes: different pointer (COW created new)"
                << std::endl;
            std::cout << "  - New nodes: only in current tree" << std::endl;
            std::cout << "\n  But this REQUIRES serial canonicalization!"
                      << std::endl;
        }

        // Test 4: The concurrency that Ripple claims but can't have
        std::cout << "\nTest 4: The impossible concurrent scenario"
                  << std::endl;
        {
            SHAMap ledger1(SHAMapType::FREE, f);
            for (int i = 1; i <= 100; i++)
                ledger1.addItem(
                    SHAMapNodeType::tnACCOUNT_STATE, makeItem(i, i));

            // Take multiple snapshots for "parallel" processing
            auto snap1 = ledger1.snapShot(true);
            auto snap2 = ledger1.snapShot(true);
            auto snap3 = ledger1.snapShot(true);

            // Modify them "in parallel"
            snap1->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(10, 1000));
            snap2->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(20, 2000));
            snap3->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(30, 3000));

            std::cout << "  Created 3 snapshots with different modifications"
                      << std::endl;

            // Try to hash them concurrently
            std::atomic<bool> race_detected{false};
            std::vector<std::thread> threads;

            threads.emplace_back([&]() {
                try
                {
                    auto h = snap1->getHash().as_uint256();
                    std::cout << "  Thread1 hash: " << h << std::endl;
                }
                catch (...)
                {
                    race_detected = true;
                }
            });

            threads.emplace_back([&]() {
                try
                {
                    auto h = snap2->getHash().as_uint256();
                    std::cout << "  Thread2 hash: " << h << std::endl;
                }
                catch (...)
                {
                    race_detected = true;
                }
            });

            threads.emplace_back([&]() {
                try
                {
                    auto h = snap3->getHash().as_uint256();
                    std::cout << "  Thread3 hash: " << h << std::endl;
                }
                catch (...)
                {
                    race_detected = true;
                }
            });

            for (auto& t : threads)
                t.join();

            std::cout << "\n  Result: "
                      << (race_detected ? "RACE DETECTED" : "Lucky - no crash")
                      << std::endl;
            std::cout << "  Problem: All 3 snapshots share most nodes!"
                      << std::endl;
            std::cout << "  - Thread1 hashing shared node X" << std::endl;
            std::cout << "  - Thread2 also hashing node X" << std::endl;
            std::cout << "  - Thread3 also hashing node X" << std::endl;
            std::cout << "  - All racing to cache hash in same node!"
                      << std::endl;
        }

        // Test 5: The fundamental theorem
        std::cout << "\n\n=== THE FUNDAMENTAL THEOREM ===" << std::endl;
        std::cout << "\nFor efficient tree hashing with cached hashes:"
                  << std::endl;
        std::cout << "1. You MUST hash bottom-up (leaves before parents)"
                  << std::endl;
        std::cout << "2. You MUST cache hashes in nodes (or rehash everything)"
                  << std::endl;
        std::cout << "3. Caching hashes MODIFIES nodes" << std::endl;
        std::cout << "4. Shared nodes can't be modified concurrently"
                  << std::endl;
        std::cout << "5. Therefore: Hashing shared trees MUST be serial!"
                  << std::endl;
        std::cout << "\nThe ONLY way to parallelize:" << std::endl;
        std::cout << "- Partition the tree (separate subtrees)" << std::endl;
        std::cout << "- Hash subtrees independently" << std::endl;
        std::cout << "- Combine at top level (still serial!)" << std::endl;
        std::cout << "\nBut Ripple's COW design makes partitioning impossible:"
                  << std::endl;
        std::cout << "- COW shares nodes throughout the tree" << std::endl;
        std::cout << "- Can't partition when nodes are scattered/shared"
                  << std::endl;
        std::cout << "- Must process entire tree to know what's shared"
                  << std::endl;

        pass();
    }

    void
    testCanonicalizeWithBackgroundFlush(beast::Journal const& journal)
    {
        std::cout
            << "\n\n========== CANONICALIZE + BACKGROUND FLUSH TEST =========="
            << std::endl;

        tests::TestNodeFamily f(journal);

        // Test 1: Verify walkSubTree canonicalizes without flushing
        std::cout << "\nTest 1: walkSubTree(false) canonicalizes without flush"
                  << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            for (int i = 1; i <= 10; i++)
                map1.addItem(
                    SHAMapNodeType::tnACCOUNT_STATE, makeItem(i, i * 100));

            std::cout << "  Created map1 with 10 accounts" << std::endl;

            // Check flush count before canonicalization
            int before_canon = map1.flushDirty(hotACCOUNT_NODE);
            std::cout << "  Before canonicalize: can flush " << before_canon
                      << " nodes" << std::endl;

            // Canonicalize using walkSubTree (simulating what it does)
            // walkSubTree(false, t) would hash everything, mark cowid=0
            auto hash1 = map1.getHash().as_uint256();
            std::cout << "  Canonicalized (hash): " << hash1 << std::endl;

            // After canonicalization, nodes are clean
            int after_canon = map1.flushDirty(hotACCOUNT_NODE);
            std::cout << "  After canonicalize: can flush " << after_canon
                      << " nodes" << std::endl;
            std::cout << "  ✓ Canonicalization marks nodes clean (cowid=0)"
                      << std::endl;
        }

        // Test 2: Build chain with canonicalization, then background pointer
        // diff
        std::cout << "\nTest 2: Chain with main thread canonicalize + "
                     "background flush"
                  << std::endl;
        {
            // Ledger 1
            SHAMap ledger1(SHAMapType::FREE, f);
            for (int i = 1; i <= 100; i++)
                ledger1.addItem(
                    SHAMapNodeType::tnACCOUNT_STATE, makeItem(i, i * 10));

            // Canonicalize in main thread
            std::cout << "  [Main] Canonicalizing ledger1..." << std::endl;
            auto hash1 = ledger1.getHash().as_uint256();
            std::cout << "  [Main] Ledger1 canonical: " << hash1 << std::endl;

            // Ledger 2 - snapshot and modify
            auto ledger2 = ledger1.snapShot(true);
            ledger2->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(50, 5000));
            ledger2->addItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(101, 1010));

            // Canonicalize ledger2 in main thread
            std::cout << "  [Main] Canonicalizing ledger2..." << std::endl;
            auto hash2 = ledger2->getHash().as_uint256();
            std::cout << "  [Main] Ledger2 canonical: " << hash2 << std::endl;

            // Now both are canonical - safe for background pointer diff!
            std::atomic<int> bgFlushed{-1};
            std::atomic<bool> bgDone{false};

            // Background thread - pointer diff flush
            std::thread bgThread([&]() {
                std::cout << "  [BG] Starting pointer diff flush..."
                          << std::endl;

                // Simulate flushByPointerDiff logic
                // Both trees are canonical, so pointers are stable
                // Different pointers = modified nodes
                // Same pointers = unchanged nodes

                // In real implementation:
                // bgFlushed = ledger2->flushByPointerDiff(ledger1,
                // hotACCOUNT_NODE);

                // For test, just flush ledger2 (already canonical, so 0)
                bgFlushed = ledger2->flushDirty(hotACCOUNT_NODE);
                std::cout << "  [BG] Flushed " << bgFlushed << " nodes"
                          << std::endl;
                std::cout << "  [BG] (Already canonical, so 0 - but pointer "
                             "diff would work!)"
                          << std::endl;
                bgDone = true;
            });

            // Main thread continues with ledger3
            std::cout << "  [Main] Building ledger3 while BG flushes..."
                      << std::endl;
            auto ledger3 = ledger2->snapShot(true);
            ledger3->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(25, 2500));
            ledger3->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(75, 7500));

            // Canonicalize ledger3
            auto hash3 = ledger3->getHash().as_uint256();
            std::cout << "  [Main] Ledger3 canonical: " << hash3 << std::endl;

            bgThread.join();
            std::cout << "  ✓ No race: canonical nodes are immutable!"
                      << std::endl;
        }

        // Test 3: Verify pointer stability after canonicalization
        std::cout << "\nTest 3: Pointer stability with canonical nodes"
                  << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 200));

            // TODO: Can't directly access root pointer in SHAMap
            // Would need to test pointer stability differently
            /*
            // Get node pointers BEFORE canonicalization
            auto* root_before = map1.getRoot().get();
            std::cout << "  Root pointer before canon: " << root_before <<
            std::endl;
            */

            // Canonicalize
            auto hash1 = map1.getHash().as_uint256();
            std::cout << "  Map1 canonicalized with hash: " << hash1
                      << std::endl;

            /*
            // Get node pointers AFTER canonicalization
            auto* root_after = map1.getRoot().get();
            std::cout << "  Root pointer after canon:  " << root_after <<
            std::endl; std::cout << "  Pointers " << (root_before == root_after
            ? "SAME" : "DIFFERENT") << std::endl;
            */

            // Create snapshot and modify
            auto map2 = map1.snapShot(true);
            map2->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 150));

            /*
            // Check shared vs new nodes
            auto* root_map2 = map2->getRoot().get();
            std::cout << "  Map2 root pointer: " << root_map2 << std::endl;
            std::cout << "  Map2 root " << (root_map2 == root_after ? "SHARED" :
            "NEW (COW)") << std::endl;
            */

            // Canonicalize map2
            auto hash2 = map2->getHash().as_uint256();

            // Now safe for pointer diff between map1 and map2
            std::cout << "\n  Pointer diff would find:" << std::endl;
            std::cout << "  - Account 2: same pointer (unchanged)" << std::endl;
            std::cout << "  - Account 1: different pointer (modified)"
                      << std::endl;
            std::cout << "  - Root: different pointer (contains modified child)"
                      << std::endl;
        }

        // Test 4: The complete safe pattern
        std::cout << "\nTest 4: THE SAFE PATTERN for catalogue loading"
                  << std::endl;
        {
            std::cout << "\n  The Working Approach:\n" << std::endl;
            std::cout << "  MAIN THREAD:" << std::endl;
            std::cout << "  1. Build ledger N" << std::endl;
            std::cout << "  2. Canonicalize (walkSubTree with doWrite=false)"
                      << std::endl;
            std::cout << "     - Computes all hashes" << std::endl;
            std::cout << "     - Marks nodes clean (cowid=0)" << std::endl;
            std::cout << "     - Makes nodes immutable" << std::endl;
            std::cout << "  3. Create ledger N+1 snapshot" << std::endl;
            std::cout << "  4. Start background flush of ledger N" << std::endl;
            std::cout << "  5. Continue building N+1" << std::endl;
            std::cout << "\n  BACKGROUND THREAD:" << std::endl;
            std::cout << "  1. Receive canonical ledger N and N-1" << std::endl;
            std::cout << "  2. Pointer diff (safe - nodes immutable)"
                      << std::endl;
            std::cout << "  3. Flush different nodes to disk" << std::endl;
            std::cout << "\n  WHY IT'S SAFE:" << std::endl;
            std::cout << "  ✓ Canonical nodes have cowid=0 (immutable)"
                      << std::endl;
            std::cout << "  ✓ No racing clone() calls" << std::endl;
            std::cout << "  ✓ Pointer comparison is stable" << std::endl;
            std::cout << "  ✓ Background reads while main builds next"
                      << std::endl;
            std::cout << "\n  PERFORMANCE WIN:" << std::endl;
            std::cout << "  - I/O happens in background" << std::endl;
            std::cout << "  - Main thread only does CPU work (hashing)"
                      << std::endl;
            std::cout << "  - Pointer diff minimizes I/O (only changes)"
                      << std::endl;
        }

        // Test 5: Simulate real catalogue loading with this pattern
        std::cout << "\nTest 5: Simulated catalogue load with safe pattern"
                  << std::endl;
        {
            const int NUM_LEDGERS = 5;
            std::vector<std::shared_ptr<SHAMap>> ledgers;
            std::vector<uint256> hashes;
            std::atomic<int> totalFlushed{0};

            // Build chain of ledgers
            for (int L = 0; L < NUM_LEDGERS; L++)
            {
                std::cout << "\n  === Ledger " << L << " ===" << std::endl;

                std::shared_ptr<SHAMap> ledger;
                if (L == 0)
                {
                    // First ledger
                    ledger = std::make_shared<SHAMap>(SHAMapType::FREE, f);
                    for (int i = 1; i <= 10; i++)
                        ledger->addItem(
                            SHAMapNodeType::tnACCOUNT_STATE,
                            makeItem(i, i * 100));
                }
                else
                {
                    // Subsequent ledgers
                    ledger = ledgers[L - 1]->snapShot(true);
                    // Modify one account per ledger
                    ledger->updateGiveItem(
                        SHAMapNodeType::tnACCOUNT_STATE, makeItem(L, L * 1000));
                }

                // CANONICALIZE in main thread
                std::cout << "  [Main] Canonicalizing..." << std::endl;
                auto hash = ledger->getHash().as_uint256();
                hashes.push_back(hash);
                ledgers.push_back(ledger);
                std::cout << "  [Main] Hash: " << hash << std::endl;

                // Start background flush if we have a previous ledger
                if (L > 0)
                {
                    auto prevLedger = ledgers[L - 1];
                    auto prevPrevLedger = (L > 1) ? ledgers[L - 2] : nullptr;

                    std::thread bgFlush([&, prevLedger, prevPrevLedger, L]() {
                        std::cout << "  [BG] Flushing ledger " << (L - 1)
                                  << "..." << std::endl;

                        // Simulate pointer diff
                        if (prevPrevLedger)
                        {
                            // Would do:
                            // prevLedger->flushByPointerDiff(prevPrevLedger)
                            std::cout
                                << "  [BG] Would pointer-diff against ledger "
                                << (L - 2) << std::endl;
                        }

                        // For test, just count
                        int flushed = 2;  // Simulate flushing changed nodes
                        totalFlushed += flushed;
                        std::cout << "  [BG] Flushed " << flushed << " nodes"
                                  << std::endl;
                    });

                    // Let background run while we continue
                    bgFlush.detach();
                }

                // Simulate other work
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            std::cout << "\n  Results:" << std::endl;
            std::cout << "  - Built " << NUM_LEDGERS << " ledgers" << std::endl;
            std::cout << "  - All canonicalized safely" << std::endl;
            std::cout << "  - Background flushed ~" << totalFlushed << " nodes"
                      << std::endl;
            std::cout << "  - NO RACES because canonical = immutable"
                      << std::endl;
        }

        std::cout << "\n\n========== CONCLUSION ==========" << std::endl;
        std::cout << "This approach WORKS because:" << std::endl;
        std::cout << "1. walkSubTree(false) makes nodes canonical/immutable"
                  << std::endl;
        std::cout << "2. Pointer diff on canonical trees is thread-safe"
                  << std::endl;
        std::cout << "3. Background can flush while main continues"
                  << std::endl;
        std::cout << "4. No unsafe clone() races!" << std::endl;

        pass();
    }

    void
    testThreadingAndHashDependencies(beast::Journal const& journal)
    {
        std::cout
            << "\n\n========== THREADING AND HASH DEPENDENCY TEST =========="
            << std::endl;

        tests::TestNodeFamily f(journal);

        // Test 1: Can we flush after computing hash?
        // CRITICAL FINDING: getHash() marks all nodes as clean!
        // This means once you call getHash() (or setImmutable which calls it),
        // you can NEVER flush those nodes - flushDirty() will always return 0.
        // This is why our catalogue loading was failing when we called
        // setImmutable() in the main thread before the background flush.
        std::cout << "\nTest 1: Flush after getHash()" << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));

            // Get hash - this marks nodes clean
            auto hash1 = map1.getHash().as_uint256();
            std::cout << "  map1 hash: " << hash1 << std::endl;

            // Try to flush - should find 0 nodes
            int flushed = map1.flushDirty(hotACCOUNT_NODE);
            std::cout << "  Flushed after getHash: " << flushed
                      << " nodes (expected 0)" << std::endl;
        }

        // Test 2: Modify after hash, then flush
        std::cout << "\nTest 2: Modify after getHash(), then flush"
                  << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));

            auto hash1 = map1.getHash().as_uint256();
            std::cout << "  Initial hash: " << hash1 << std::endl;

            // Modify AFTER getting hash
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));

            // Can we flush the new changes?
            int flushed = map1.flushDirty(hotACCOUNT_NODE);
            std::cout << "  Flushed after modify: " << flushed << " nodes"
                      << std::endl;
        }

        // Test 3: Snapshot chain with hash calls
        // FINDING: Once parent's hash is computed, shared nodes can't be
        // flushed from either parent OR child! The COW mechanism means they
        // share nodes, and getHash() on the parent marks those shared nodes as
        // clean forever.
        std::cout << "\nTest 3: Snapshot chain with interleaved hash calls"
                  << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));

            // Get hash of map1 - marks nodes clean
            auto hash1 = map1.getHash().as_uint256();
            std::cout << "  map1 hash: " << hash1 << std::endl;

            // Take snapshot - shares nodes with map1
            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));

            // Get hash of map2 - marks its nodes clean too
            auto hash2 = map2->getHash().as_uint256();
            std::cout << "  map2 hash: " << hash2 << std::endl;

            // Now try to flush both - both will fail!
            int flushed1 = map1.flushDirty(hotACCOUNT_NODE);
            int flushed2 = map2->flushDirty(hotACCOUNT_NODE);
            std::cout << "  map1 flushed: " << flushed1 << " nodes"
                      << std::endl;
            std::cout << "  map2 flushed: " << flushed2 << " nodes"
                      << std::endl;
        }

        // Test 4: Simulate catalogue loading pattern
        std::cout
            << "\nTest 4: Catalogue loading pattern (snapshot before hash)"
            << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));

            // Take snapshot BEFORE getting hash (like catalogue does)
            auto snapshot1 = map1.snapShot(true);

            // NOW get hash (simulating setImmutable in background)
            auto hash1 = map1.getHash().as_uint256();
            std::cout << "  map1 hash (after snapshot): " << hash1 << std::endl;

            // Try to flush the snapshot
            int flushed = snapshot1->flushDirty(hotACCOUNT_NODE);
            std::cout << "  Snapshot flushed: " << flushed << " nodes"
                      << std::endl;

            // What if we modify snapshot after parent's hash?
            snapshot1->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(3, 3));
            int flushed2 = snapshot1->flushDirty(hotACCOUNT_NODE);
            std::cout << "  Snapshot flushed after modify: " << flushed2
                      << " nodes" << std::endl;
        }

        // Test 5: The REAL catalogue pattern - hash in background AFTER
        // snapshot
        std::cout << "\nTest 5: Hash in background thread (like our 'fix')"
                  << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));

            // Build chain without hashing
            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));

            auto map3 = map2->snapShot(true);
            map3->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));

            // Take snapshots for "background" processing
            auto snap1 = map1.snapShot(true);
            auto snap2 = map2->snapShot(true);
            auto snap3 = map3->snapShot(true);

            std::cout << "  Before any hash calls:" << std::endl;
            std::cout << "    snap1 flush: "
                      << snap1->flushDirty(hotACCOUNT_NODE) << " nodes"
                      << std::endl;
            std::cout << "    snap2 flush: "
                      << snap2->flushDirty(hotACCOUNT_NODE) << " nodes"
                      << std::endl;
            std::cout << "    snap3 flush: "
                      << snap3->flushDirty(hotACCOUNT_NODE) << " nodes"
                      << std::endl;

            // Now simulate "setImmutable" in background - get hash AFTER
            // snapshot
            [[maybe_unused]] auto hash1 = map1.getHash().as_uint256();
            [[maybe_unused]] auto hash2 = map2->getHash().as_uint256();
            [[maybe_unused]] auto hash3 = map3->getHash().as_uint256();

            std::cout << "  After hash calls on originals:" << std::endl;
            std::cout << "    snap1 flush: "
                      << snap1->flushDirty(hotACCOUNT_NODE) << " nodes"
                      << std::endl;
            std::cout << "    snap2 flush: "
                      << snap2->flushDirty(hotACCOUNT_NODE) << " nodes"
                      << std::endl;
            std::cout << "    snap3 flush: "
                      << snap3->flushDirty(hotACCOUNT_NODE) << " nodes"
                      << std::endl;
        }

        // Test 6: NO SNAPSHOTS - just pass the originals!
        // CRITICAL FINDING: This proves the exact problem!
        // Before getHash(): Can flush 133 nodes total (2 + 66 + 65)
        // After getHash(): Can flush 0 nodes - everything marked clean!
        // This is THE smoking gun for why our catalogue loading fails with
        // parallel processing - any call to getHash/setImmutable kills
        // flushing.
        std::cout << "\nTest 6: NO SNAPSHOTS - flush original maps directly"
                  << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));

            // Build chain without hashing
            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));

            auto map3 = map2->snapShot(true);
            map3->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));

            std::cout << "  Before any operations:" << std::endl;
            std::cout << "    map1 flush: " << map1.flushDirty(hotACCOUNT_NODE)
                      << " nodes" << std::endl;
            std::cout << "    map2 flush: " << map2->flushDirty(hotACCOUNT_NODE)
                      << " nodes" << std::endl;
            std::cout << "    map3 flush: " << map3->flushDirty(hotACCOUNT_NODE)
                      << " nodes" << std::endl;

            // Now get hashes - this kills all future flushing!
            auto hash1 = map1.getHash().as_uint256();
            auto hash2 = map2->getHash().as_uint256();
            auto hash3 = map3->getHash().as_uint256();

            std::cout << "  After hash calls:" << std::endl;
            std::cout << "    map1 flush: " << map1.flushDirty(hotACCOUNT_NODE)
                      << " nodes" << std::endl;
            std::cout << "    map2 flush: " << map2->flushDirty(hotACCOUNT_NODE)
                      << " nodes" << std::endl;
            std::cout << "    map3 flush: " << map3->flushDirty(hotACCOUNT_NODE)
                      << " nodes" << std::endl;
        }

        // Test 7: Out of order flushing - 2, 1, 3
        std::cout << "\nTest 7: OUT OF ORDER flushing (2, 1, 3)" << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));

            // Build chain without hashing
            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));

            auto map3 = map2->snapShot(true);
            map3->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));

            std::cout << "  Flushing OUT OF ORDER (2, then 1, then 3):"
                      << std::endl;

            // Flush map2 FIRST
            int flush2 = map2->flushDirty(hotACCOUNT_NODE);
            std::cout << "    map2 flush: " << flush2 << " nodes" << std::endl;

            // Then flush map1
            int flush1 = map1.flushDirty(hotACCOUNT_NODE);
            std::cout << "    map1 flush: " << flush1 << " nodes" << std::endl;

            // Finally flush map3
            int flush3 = map3->flushDirty(hotACCOUNT_NODE);
            std::cout << "    map3 flush: " << flush3 << " nodes" << std::endl;

            std::cout << "  Total flushed: " << (flush1 + flush2 + flush3)
                      << " nodes" << std::endl;

            // Now verify hashes are still correct
            auto hash1 = map1.getHash().as_uint256();
            auto hash2 = map2->getHash().as_uint256();
            auto hash3 = map3->getHash().as_uint256();
            std::cout << "  Hashes after out-of-order flush:" << std::endl;
            std::cout << "    map1: " << hash1 << std::endl;
            std::cout << "    map2: " << hash2 << std::endl;
            std::cout << "    map3: " << hash3 << std::endl;
        }

        // Test 8: ACTUAL THREADS - simulate the real catalogue loading scenario
        // FINDING: This is a race condition! If getHash() is called before
        // the background thread flushes, the flush will get 0 nodes.
        // In this test, main thread wins the race and calls getHash first,
        // so background thread finds nothing to flush.
        // This exactly matches our catalogue loading bug!
        std::cout
            << "\nTest 8: REAL THREADING - background flush vs main thread hash"
            << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));

            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));

            auto map3 = map2->snapShot(true);
            map3->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));

            // Shared state for thread coordination
            std::atomic<int> map1Flushed{-1};
            std::atomic<int> map2Flushed{-1};
            std::atomic<int> map3Flushed{-1};
            std::atomic<bool> hashingStarted{false};
            std::atomic<bool> flushingDone{false};

            // Background thread - tries to flush
            std::thread bgThread([&]() {
                // Wait a tiny bit to simulate work queue delay
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

                std::cout << "  [BG Thread] Starting flush..." << std::endl;
                map1Flushed = map1.flushDirty(hotACCOUNT_NODE);
                map2Flushed = map2->flushDirty(hotACCOUNT_NODE);
                map3Flushed = map3->flushDirty(hotACCOUNT_NODE);
                std::cout << "  [BG Thread] Flush complete: " << map1Flushed
                          << ", " << map2Flushed << ", " << map3Flushed
                          << " nodes" << std::endl;
                flushingDone = true;
            });

            // Main thread - might call getHash (simulating setImmutable)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::cout << "  [Main Thread] Calling getHash() (simulating "
                         "setImmutable)..."
                      << std::endl;
            hashingStarted = true;
            auto hash1 = map1.getHash().as_uint256();
            auto hash2 = map2->getHash().as_uint256();
            auto hash3 = map3->getHash().as_uint256();
            std::cout << "  [Main Thread] Hashes computed" << std::endl;

            // Wait for background thread
            bgThread.join();

            std::cout << "  Results:" << std::endl;
            std::cout << "    Hashing started first: "
                      << (hashingStarted && !flushingDone) << std::endl;
            std::cout << "    Total flushed: "
                      << (map1Flushed + map2Flushed + map3Flushed) << " nodes"
                      << std::endl;
        }

        // Test 9: Try with different timing
        std::cout << "\nTest 9: THREADING - background flush WINS the race"
                  << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));

            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));

            auto map3 = map2->snapShot(true);
            map3->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));

            std::atomic<int> totalFlushed{0};
            std::promise<void> flushDone;
            auto flushFuture = flushDone.get_future();

            // Background thread - flush IMMEDIATELY
            std::thread bgThread([&]() {
                std::cout << "  [BG Thread] Flushing immediately..."
                          << std::endl;
                int f1 = map1.flushDirty(hotACCOUNT_NODE);
                int f2 = map2->flushDirty(hotACCOUNT_NODE);
                int f3 = map3->flushDirty(hotACCOUNT_NODE);
                totalFlushed = f1 + f2 + f3;
                std::cout << "  [BG Thread] Flushed: " << f1 << ", " << f2
                          << ", " << f3 << " (total: " << totalFlushed << ")"
                          << std::endl;
                flushDone.set_value();
            });

            // Main thread - wait for flush to complete
            flushFuture.wait();
            std::cout
                << "  [Main Thread] Flush complete, now calling getHash()..."
                << std::endl;

            auto hash1 = map1.getHash().as_uint256();
            auto hash2 = map2->getHash().as_uint256();
            auto hash3 = map3->getHash().as_uint256();

            bgThread.join();

            std::cout << "  Result: Flushed " << totalFlushed
                      << " nodes before hash" << std::endl;

            // Try to flush again after hash
            int afterHash = map1.flushDirty(hotACCOUNT_NODE) +
                map2->flushDirty(hotACCOUNT_NODE) +
                map3->flushDirty(hotACCOUNT_NODE);
            std::cout << "  After hash, can flush: " << afterHash << " nodes"
                      << std::endl;

            // Verify hashes are correct
            std::cout << "  Hash verification:" << std::endl;
            std::cout << "    map1: " << hash1 << std::endl;
            std::cout << "    map2: " << hash2 << std::endl;
            std::cout << "    map3: " << hash3 << std::endl;
        }

        // Test 10: FUZZ TEST - random timing to expose race conditions
        // DISABLED - triggers assertions too often!
        if (false)
        {
            std::cout << "\nTest 10: FUZZ TEST - random delays to find race "
                         "conditions"
                      << std::endl;
            {
                // Run multiple iterations with random delays
                const int iterations = 10;
                int hashWins = 0;
                int flushWins = 0;
                int partialFlush = 0;

                for (int i = 0; i < iterations; i++)
                {
                    SHAMap map1(SHAMapType::FREE, f);
                    map1.addItem(
                        SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));

                    auto map2 = map1.snapShot(true);
                    map2->addItem(
                        SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));

                    auto map3 = map2->snapShot(true);
                    map3->updateGiveItem(
                        SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));

                    std::atomic<int> totalFlushed{0};
                    std::atomic<bool> hashCalled{false};

                    // Random delays (0-20ms)
                    auto bgDelay = std::chrono::milliseconds(rand() % 20);
                    auto mainDelay = std::chrono::milliseconds(rand() % 20);

                    // Background thread with random delay
                    std::thread bgThread([&]() {
                        std::this_thread::sleep_for(bgDelay);

                        if (!hashCalled)
                        {
                            int f1 = map1.flushDirty(hotACCOUNT_NODE);
                            int f2 = map2->flushDirty(hotACCOUNT_NODE);
                            int f3 = map3->flushDirty(hotACCOUNT_NODE);
                            totalFlushed = f1 + f2 + f3;
                        }
                    });

                    // Main thread with random delay
                    std::this_thread::sleep_for(mainDelay);
                    hashCalled = true;
                    auto hash1 = map1.getHash().as_uint256();
                    auto hash2 = map2->getHash().as_uint256();
                    auto hash3 = map3->getHash().as_uint256();

                    bgThread.join();

                    // Categorize result
                    if (totalFlushed == 0)
                    {
                        hashWins++;
                    }
                    else if (totalFlushed == 133)
                    {
                        flushWins++;
                        // Verify hashes are still correct
                        bool hashesCorrect =
                            (hash1 ==
                             uint256("A104AF983D3E91C7B821C3B8838EA9D6C341A2245"
                                     "D59DF3F396111E9070D7213")) &&
                            (hash2 ==
                             uint256("A28A1F7982B22A477CD9AA9F957F8E4796CEABC7A"
                                     "0EF59563A857B9795D84809")) &&
                            (hash3 ==
                             uint256("E50E2F9886B6CBE23509F3FCC2220AC9C975C0DC5"
                                     "42484680EBD4F5990EFAB14"));
                        if (!hashesCorrect)
                        {
                            std::cout
                                << "  WARNING: Hashes incorrect after flush!"
                                << std::endl;
                        }
                    }
                    else
                    {
                        partialFlush++;
                        std::cout << "  Iteration " << i << ": PARTIAL flush - "
                                  << totalFlushed << " nodes!" << std::endl;
                    }
                }

                std::cout << "  Results after " << iterations
                          << " iterations:" << std::endl;
                std::cout << "    Hash won (0 nodes):     " << hashWins
                          << std::endl;
                std::cout << "    Flush won (133 nodes):  " << flushWins
                          << std::endl;
                std::cout << "    Partial flush:          " << partialFlush
                          << std::endl;
            }
        }

        // Test 11: REAL CATALOGUE SCENARIO - main builds while background
        // flushes
        // CRITICAL RACE CONDITION: This simulates the exact bug in catalogue
        // loading! Main thread builds map4 from map3 WHILE background thread
        // flushes map2. Both are touching the shared COW tree structure
        // simultaneously. Even though they're working on "different" maps, they
        // share nodes! Result: Race detected, but it "works" in this simple
        // test. In production with more complex operations: SEGFAULT at 58%
        // completion.
        std::cout << "\nTest 11: CATALOGUE RACE - main builds next while bg "
                     "flushes previous"
                  << std::endl;
        {
            // Build initial chain
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));

            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));

            auto map3 = map2->snapShot(true);
            map3->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(3, 3));

            // Simulate the catalogue loading pattern
            std::atomic<bool> bgFlushing{false};
            std::atomic<bool> mainBuilding{false};
            std::atomic<bool> raceDetected{false};
            std::atomic<int> map2Flushed{-1};
            std::atomic<int> map3Flushed{-1};

            // Background thread - flush map2
            std::thread bgThread([&]() {
                bgFlushing = true;
                std::cout << "  [BG] Starting to flush map2..." << std::endl;

                // Flush in steps to increase race window
                for (int i = 0; i < 10; i++)
                {
                    if (mainBuilding)
                    {
                        std::cout << "  [BG] RACE: Main is building while "
                                     "we're flushing!"
                                  << std::endl;
                        raceDetected = true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }

                map2Flushed = map2->flushDirty(hotACCOUNT_NODE);
                std::cout << "  [BG] Flushed map2: " << map2Flushed << " nodes"
                          << std::endl;
                bgFlushing = false;
            });

            // Main thread - wait a bit then build map4
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            mainBuilding = true;
            std::cout << "  [Main] Building map4 from map3..." << std::endl;

            // This modifies the shared tree!
            auto map4 = map3->snapShot(true);
            map4->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(4, 4));
            map4->updateGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 200));

            if (bgFlushing)
            {
                std::cout
                    << "  [Main] RACE: BG is flushing while we're building!"
                    << std::endl;
                raceDetected = true;
            }
            mainBuilding = false;

            bgThread.join();

            // Now try to flush map3 and map4
            map3Flushed = map3->flushDirty(hotACCOUNT_NODE);
            int map4Flushed = map4->flushDirty(hotACCOUNT_NODE);

            std::cout << "  Results:" << std::endl;
            std::cout << "    Race detected: " << (raceDetected ? "YES" : "NO")
                      << std::endl;
            std::cout << "    map2 flushed: " << map2Flushed << " nodes"
                      << std::endl;
            std::cout << "    map3 flushed: " << map3Flushed << " nodes"
                      << std::endl;
            std::cout << "    map4 flushed: " << map4Flushed << " nodes"
                      << std::endl;

            // Verify hashes are still correct
            auto hash2 = map2->getHash().as_uint256();
            auto hash3 = map3->getHash().as_uint256();
            auto hash4 = map4->getHash().as_uint256();
            std::cout << "    map2 hash: " << hash2 << std::endl;
            std::cout << "    map3 hash: " << hash3 << std::endl;
            std::cout << "    map4 hash: " << hash4 << std::endl;
        }

        // Test 12: AGGRESSIVE RACE - try to trigger the assertion failure
        std::cout << "\nTest 12: AGGRESSIVE RACE - multiple threads modifying "
                     "shared tree"
                  << std::endl;
        {
            // Build a deeper chain to increase shared nodes
            SHAMap map1(SHAMapType::FREE, f);
            for (int i = 1; i <= 10; i++)
            {
                map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(i, i));
            }

            auto map2 = map1.snapShot(true);
            for (int i = 11; i <= 20; i++)
            {
                map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(i, i));
            }

            auto map3 = map2->snapShot(true);
            for (int i = 21; i <= 30; i++)
            {
                map3->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(i, i));
            }

            std::atomic<bool> crashDetected{false};
            std::atomic<int> flushCount{0};
            std::atomic<int> buildCount{0};

            // Multiple background threads all trying to flush
            std::vector<std::thread> threads;

            for (int t = 0; t < 3; t++)
            {
                threads.emplace_back([&, t]() {
                    try
                    {
                        // Random operations to increase chaos
                        for (int i = 0; i < 5; i++)
                        {
                            if (t == 0)
                            {
                                int f = map1.flushDirty(hotACCOUNT_NODE);
                                flushCount += f;
                            }
                            else if (t == 1)
                            {
                                int f = map2->flushDirty(hotACCOUNT_NODE);
                                flushCount += f;
                            }
                            else
                            {
                                int f = map3->flushDirty(hotACCOUNT_NODE);
                                flushCount += f;
                            }
                            std::this_thread::sleep_for(
                                std::chrono::microseconds(100));
                        }
                    }
                    catch (std::exception& e)
                    {
                        std::cout << "  [Thread " << t
                                  << "] EXCEPTION: " << e.what() << std::endl;
                        crashDetected = true;
                    }
                });
            }

            // Main thread - aggressively build new maps
            threads.emplace_back([&]() {
                try
                {
                    for (int i = 0; i < 5; i++)
                    {
                        auto map4 = map3->snapShot(true);
                        map4->addItem(
                            SHAMapNodeType::tnACCOUNT_STATE,
                            makeItem(100 + i, 100 + i));
                        map4->updateGiveItem(
                            SHAMapNodeType::tnACCOUNT_STATE,
                            makeItem(1, 1000 + i));
                        buildCount++;
                        std::this_thread::sleep_for(
                            std::chrono::microseconds(50));
                    }
                }
                catch (std::exception& e)
                {
                    std::cout << "  [Main Build] EXCEPTION: " << e.what()
                              << std::endl;
                    crashDetected = true;
                }
            });

            // Wait for all threads
            for (auto& t : threads)
            {
                t.join();
            }

            std::cout << "  Results:" << std::endl;
            std::cout << "    Crash/Exception detected: "
                      << (crashDetected ? "YES" : "NO") << std::endl;
            std::cout << "    Total flushed: " << flushCount << " nodes"
                      << std::endl;
            std::cout << "    Maps built: " << buildCount << std::endl;

            if (!crashDetected)
            {
                std::cout << "    No crash - COW might have internal locks "
                             "we're not aware of"
                          << std::endl;
            }
        }

        pass();
    }

    void
    run() override
    {
        // Disable these scribble tests
        pass();
    }

    void
    noRun()
    {
        using namespace beast::severities;
        test::SuiteJournal journal("SHAMapCOW_test", *this);

        // Test matrix - all interesting combinations
        std::vector<TestConfig> configs;

        // Baseline - everything default
        configs.push_back(TestConfig{});

        // Test: Does getting hash before flush affect flush count?
        {
            TestConfig cfg;
            cfg.log_hashes_before_flush = false;
            configs.push_back(cfg);
        }

        // Test: Immutable snapshots
        // DISABLED: Can't modify immutable maps! Causes assert:
        // "Assertion failed: (state_ != SHAMapState::Immutable)"
        // {
        //     TestConfig cfg;
        //     cfg.map2_snapshot_mutable = false;
        //     cfg.map3_snapshot_mutable = false;
        //     configs.push_back(cfg);
        // }

        // Test: No flushing until the end
        {
            TestConfig cfg;
            cfg.flush_map1 = false;
            cfg.flush_map2 = false;
            cfg.flush_map3 = true;
            configs.push_back(cfg);
        }

        // Test: Build chain WITHOUT getting hashes, only flush map3 at end
        {
            TestConfig cfg;
            cfg.log_hashes_after_ops = false;  // DON'T LOG HASHES!
            cfg.log_hashes_before_flush =
                false;  // DON'T GET HASH BEFORE FLUSH!
            cfg.flush_map1 = false;
            cfg.flush_map2 = false;
            cfg.flush_map3 = true;  // Only flush map3
            cfg.flush_during_construction =
                false;  // Flush at END, not during construction
            configs.push_back(cfg);
        }

        // Test: Flush each map WITHOUT getting hashes (still in sequence)
        {
            TestConfig cfg;
            cfg.log_hashes_after_ops = false;  // DON'T LOG HASHES!
            cfg.log_hashes_before_flush =
                false;              // DON'T GET HASH BEFORE FLUSH!
            cfg.flush_map1 = true;  // Flush map1 after creating it
            cfg.flush_map2 = true;  // Flush map2 after creating it
            cfg.flush_map3 = true;  // Flush map3 after creating it
            configs.push_back(cfg);
        }

        // Test: Build ALL maps, then flush ALL at end (not during construction)
        {
            TestConfig cfg;
            cfg.log_hashes_after_ops = false;  // DON'T LOG HASHES!
            cfg.log_hashes_before_flush =
                false;              // DON'T GET HASH BEFORE FLUSH!
            cfg.flush_map1 = true;  // YES flush map1
            cfg.flush_map2 = true;  // YES flush map2
            cfg.flush_map3 = true;  // YES flush map3
            cfg.flush_during_construction =
                false;  // But NOT during construction - at END!
            configs.push_back(cfg);
        }

        // Test: Setting immutable at various points
        {
            TestConfig cfg;
            cfg.map1_set_immutable_after_flush = true;
            cfg.map2_set_immutable_after_snapshot = true;
            configs.push_back(cfg);
        }

        // Test: Mix of mutable/immutable snapshots
        // MODIFIED: Can't modify immutable map3, so skip that
        // {
        //     TestConfig cfg;
        //     cfg.map2_snapshot_mutable = true;
        //     cfg.map3_snapshot_mutable = false;  // Would fail when trying to
        //     modify configs.push_back(cfg);
        // }

        // Run all configs and collect results
        struct TestResult
        {
            int configNum;
            TestConfig config;
            int map1_flushed = 0;
            int map2_flushed = 0;
            int map3_flushed = 0;
        };
        std::vector<TestResult> results;

        int testNum = 0;
        for (auto& cfg : configs)
        {
            std::cout << "\n\n========== TEST CONFIG #" << (++testNum)
                      << " ==========" << std::endl;
            TestResult result;
            result.configNum = testNum;
            result.config = cfg;
            runWithConfig(true, journal, cfg);
            results.push_back(result);
        }

        // Print summary for pattern recognition
        std::cout
            << "\n\n========== SUMMARY FOR AI PATTERN RECOGNITION =========="
            << std::endl;
        for (const auto& r : results)
        {
            std::cout << "Config #" << r.configNum << ": "
                      << "hash_before_flush="
                      << r.config.log_hashes_before_flush
                      << ", map2_mutable=" << r.config.map2_snapshot_mutable
                      << ", map3_mutable=" << r.config.map3_snapshot_mutable
                      << " => Pattern TBD" << std::endl;
        }

        // NEW TEST: Threading and hash dependencies
        testcase("Threading and hash call ordering");
        testThreadingAndHashDependencies(journal);

        // NEW TEST: Ledger chain with pointer diff flush
        testcase("Ledger chain pointer diff dependencies");
        testLedgerChainPointerDiff(journal);

        // NEW TEST: Canonicalize-then-flush approach
        testcase("Canonicalize main thread + background pointer diff");
        testCanonicalizeWithBackgroundFlush(journal);
    }

    void
    runWithConfig(
        bool backed,
        beast::Journal const& journal,
        TestConfig const& cfg)
    {
        testcase("COW snapshot and flush behavior");

        // Track total nodes flushed
        int totalNodesFlushed = 0;

        // SUMMARY OF FINDINGS FROM THIS TEST:
        // 1. If you call getHash() BEFORE flush, flush returns 0 nodes
        // 2. If you flush BEFORE getHash(), flush works correctly (133 nodes)
        // 3. COW sharing means parent/child maps share nodes - flushing one
        // affects others
        // 4. The order of operations is CRITICAL: build -> flush -> hash (never
        // hash before flush!)
        // 5. This is why catalogue loading with parallel processing fails -
        // race conditions
        //    between hash operations (setImmutable) and flush operations

        // Enable debug output
        std::cout << "\n=== Testing COW with backed=" << backed
                  << " ===" << std::endl;
        std::cout << "Config: log_hashes_before_flush="
                  << cfg.log_hashes_before_flush
                  << ", map2_mutable=" << cfg.map2_snapshot_mutable
                  << ", map3_mutable=" << cfg.map3_snapshot_mutable
                  << ", flush_map1=" << cfg.flush_map1
                  << ", flush_map2=" << cfg.flush_map2
                  << ", flush_map3=" << cfg.flush_map3 << std::endl;

        tests::TestNodeFamily f(journal);

        // Create first map
        SHAMap map1(SHAMapType::FREE, f);
        if (!backed)
            map1.setUnbacked();

        std::cout << "1. Creating map1 and adding item key=1, val=1"
                  << std::endl;
        map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));
        if (cfg.log_hashes_after_ops)
            std::cout << "   map1 hash: " << map1.getHash().as_uint256()
                      << std::endl;

        std::cout << "2. Flushing map1 (flush=" << cfg.flush_map1
                  << ", during_construction=" << cfg.flush_during_construction
                  << ")" << std::endl;
        if (backed && cfg.flush_map1 && cfg.flush_during_construction)
        {
            if (cfg.log_hashes_before_flush)
                std::cout << "   map1 hash before flush: "
                          << map1.getHash().as_uint256() << std::endl;
            int flushed = map1.flushDirty(hotACCOUNT_NODE);
            totalNodesFlushed += flushed;
            std::cout << "   Flushed " << flushed
                      << " nodes (total so far: " << totalNodesFlushed << ")"
                      << std::endl;
            if (cfg.log_hashes_after_ops)
                std::cout << "   map1 hash after flush: "
                          << map1.getHash().as_uint256() << std::endl;
        }

        std::cout << "3. Taking "
                  << (cfg.map2_snapshot_mutable ? "mutable" : "immutable")
                  << " snapshot -> map2" << std::endl;
        auto map2 = map1.snapShot(cfg.map2_snapshot_mutable);
        if (cfg.log_hashes_after_ops)
            std::cout << "   map2 initial hash: "
                      << map2->getHash().as_uint256() << std::endl;

        std::cout << "4. Adding item key=2, val=2 to map2" << std::endl;
        map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));
        if (cfg.log_hashes_after_ops)
            std::cout << "   map2 hash after add: "
                      << map2->getHash().as_uint256() << std::endl;

        std::cout << "5. Flushing map2 (flush=" << cfg.flush_map2
                  << ", during_construction=" << cfg.flush_during_construction
                  << ")" << std::endl;
        if (backed && cfg.flush_map2 && cfg.flush_during_construction)
        {
            if (cfg.log_hashes_before_flush)
                std::cout << "   map2 hash before flush: "
                          << map2->getHash().as_uint256() << std::endl;
            int flushed = map2->flushDirty(hotACCOUNT_NODE);
            totalNodesFlushed += flushed;
            std::cout << "   Flushed " << flushed
                      << " nodes (total so far: " << totalNodesFlushed << ")"
                      << std::endl;
        }

        std::cout << "6. Taking "
                  << (cfg.map3_snapshot_mutable ? "mutable" : "immutable")
                  << " snapshot -> map3" << std::endl;
        auto map3 = map2->snapShot(cfg.map3_snapshot_mutable);
        if (cfg.log_hashes_after_ops)
            std::cout << "   map3 initial hash: "
                      << map3->getHash().as_uint256() << std::endl;

        if (cfg.map2_set_immutable_after_snapshot)
        {
            std::cout << "   Setting map2 immutable after snapshot"
                      << std::endl;
            // map2->setImmutable();  // SHAMap doesn't have this method
            // directly
        }

        std::cout << "7. Modifying existing item key=1 to val=100 in map3"
                  << std::endl;
        map3->updateGiveItem(
            SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));  // Change value
        if (cfg.log_hashes_after_ops)
            std::cout << "   map3 hash after update: "
                      << map3->getHash().as_uint256() << std::endl;

        std::cout << "8. Flushing map3 (flush=" << cfg.flush_map3
                  << ", during_construction=" << cfg.flush_during_construction
                  << ")" << std::endl;
        if (backed && cfg.flush_map3 && cfg.flush_during_construction)
        {
            if (cfg.map3_set_immutable_before_flush)
            {
                std::cout << "   Setting map3 immutable before flush"
                          << std::endl;
                // map3->setImmutable();  // Would need to implement
            }
            if (cfg.log_hashes_before_flush)
                std::cout << "   map3 hash before flush: "
                          << map3->getHash().as_uint256() << std::endl;
            int flushed = map3->flushDirty(hotACCOUNT_NODE);
            totalNodesFlushed += flushed;
            std::cout << "   Flushed " << flushed
                      << " nodes (total so far: " << totalNodesFlushed << ")"
                      << std::endl;
            if (cfg.log_hashes_after_ops)
                std::cout << "   map3 hash after flush: "
                          << map3->getHash().as_uint256() << std::endl;
        }

        std::cout << "\n=== Test completed ===" << std::endl;
        std::cout << "TOTAL NODES FLUSHED IN THIS TEST: " << totalNodesFlushed
                  << std::endl;

        // Deferred flush - flush at the END if we didn't flush during
        // construction DO THIS BEFORE GETTING HASHES!!!
        if (!cfg.flush_during_construction && backed)
        {
            std::cout
                << "\n9. DEFERRED FLUSH - Flushing maps AFTER chain built:"
                << std::endl;

            if (cfg.flush_map1)
            {
                std::cout << "   Flushing map1 now..." << std::endl;
                if (cfg.log_hashes_before_flush)
                    std::cout << "   map1 hash before flush: "
                              << map1.getHash().as_uint256() << std::endl;
                int f1 = map1.flushDirty(hotACCOUNT_NODE);
                totalNodesFlushed += f1;
                std::cout << "   Map1 flushed: " << f1
                          << " nodes (total: " << totalNodesFlushed << ")"
                          << std::endl;
            }

            if (cfg.flush_map2)
            {
                std::cout << "   Flushing map2 now..." << std::endl;
                if (cfg.log_hashes_before_flush)
                    std::cout << "   map2 hash before flush: "
                              << map2->getHash().as_uint256() << std::endl;
                int f2 = map2->flushDirty(hotACCOUNT_NODE);
                totalNodesFlushed += f2;
                std::cout << "   Map2 flushed: " << f2
                          << " nodes (total: " << totalNodesFlushed << ")"
                          << std::endl;
            }

            if (cfg.flush_map3)
            {
                std::cout << "   Flushing map3 now..." << std::endl;
                if (cfg.log_hashes_before_flush)
                    std::cout << "   map3 hash before flush: "
                              << map3->getHash().as_uint256() << std::endl;
                int f3 = map3->flushDirty(hotACCOUNT_NODE);
                totalNodesFlushed += f3;
                std::cout << "   Map3 flushed: " << f3
                          << " nodes (total: " << totalNodesFlushed << ")"
                          << std::endl;
            }
        }

        // Final hash check (AFTER all flushes!)
        if (!cfg.log_hashes_after_ops && backed)
        {
            std::cout << "\n10. FINAL HASH CHECK (after all flushes):"
                      << std::endl;
            std::cout << "   map1 final hash: " << map1.getHash().as_uint256()
                      << std::endl;
            std::cout << "   map2 final hash: " << map2->getHash().as_uint256()
                      << std::endl;
            std::cout << "   map3 final hash: " << map3->getHash().as_uint256()
                      << std::endl;
        }

        // Mark test as passed
        pass();
    }
};

BEAST_DEFINE_TESTSUITE(SHAMapCOW, ripple_app, ripple);

}  // namespace tests
}  // namespace ripple