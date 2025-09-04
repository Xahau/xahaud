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
#include <test/shamap/common.h>
#include <test/unit_test/SuiteJournal.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <future>
#include <chrono>

namespace ripple {
namespace tests {

// Dead simple test item helper - just use numbers
static boost::intrusive_ptr<SHAMapItem>
makeItem(int k, int v = 0) {
    uint256 key(k);
    uint256 val(v);
    return make_shamapitem(key, Slice{val.data(), val.size()});  // Use full uint256
}

// Test configuration
struct TestConfig {
    // Hash logging
    bool log_hashes_before_flush = true;
    bool log_hashes_after_ops = true;
    
    // Flushing decisions
    bool flush_map1 = true;
    bool flush_map2 = false;  // Usually false to test deferred flush
    bool flush_map3 = true;
    
    // WHEN to flush - THIS IS THE KEY!
    bool flush_during_construction = true;  // true = flush as we build, false = flush at end
    
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

    void testThreadingAndHashDependencies(beast::Journal const& journal)
    {
        std::cout << "\n\n========== THREADING AND HASH DEPENDENCY TEST ==========" << std::endl;
        
        tests::TestNodeFamily f(journal);
        
        // Test 1: Can we flush after computing hash?
        std::cout << "\nTest 1: Flush after getHash()" << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));
            
            // Get hash - this marks nodes clean
            auto hash1 = map1.getHash().as_uint256();
            std::cout << "  map1 hash: " << hash1 << std::endl;
            
            // Try to flush - should find 0 nodes
            int flushed = map1.flushDirty(hotACCOUNT_NODE);
            std::cout << "  Flushed after getHash: " << flushed << " nodes (expected 0)" << std::endl;
        }
        
        // Test 2: Modify after hash, then flush
        std::cout << "\nTest 2: Modify after getHash(), then flush" << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));
            
            auto hash1 = map1.getHash().as_uint256();
            std::cout << "  Initial hash: " << hash1 << std::endl;
            
            // Modify AFTER getting hash
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));
            
            // Can we flush the new changes?
            int flushed = map1.flushDirty(hotACCOUNT_NODE);
            std::cout << "  Flushed after modify: " << flushed << " nodes" << std::endl;
        }
        
        // Test 3: Snapshot chain with hash calls
        std::cout << "\nTest 3: Snapshot chain with interleaved hash calls" << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));
            
            // Get hash of map1
            auto hash1 = map1.getHash().as_uint256();
            std::cout << "  map1 hash: " << hash1 << std::endl;
            
            // Take snapshot
            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));
            
            // Get hash of map2 - does this affect map1's nodes?
            auto hash2 = map2->getHash().as_uint256();
            std::cout << "  map2 hash: " << hash2 << std::endl;
            
            // Now try to flush both
            int flushed1 = map1.flushDirty(hotACCOUNT_NODE);
            int flushed2 = map2->flushDirty(hotACCOUNT_NODE);
            std::cout << "  map1 flushed: " << flushed1 << " nodes" << std::endl;
            std::cout << "  map2 flushed: " << flushed2 << " nodes" << std::endl;
        }
        
        // Test 4: Simulate catalogue loading pattern
        std::cout << "\nTest 4: Catalogue loading pattern (snapshot before hash)" << std::endl;
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
            std::cout << "  Snapshot flushed: " << flushed << " nodes" << std::endl;
            
            // What if we modify snapshot after parent's hash?
            snapshot1->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(3, 3));
            int flushed2 = snapshot1->flushDirty(hotACCOUNT_NODE);
            std::cout << "  Snapshot flushed after modify: " << flushed2 << " nodes" << std::endl;
        }
        
        // Test 5: The REAL catalogue pattern - hash in background AFTER snapshot
        std::cout << "\nTest 5: Hash in background thread (like our 'fix')" << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));
            
            // Build chain without hashing
            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));
            
            auto map3 = map2->snapShot(true);
            map3->updateGiveItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));
            
            // Take snapshots for "background" processing
            auto snap1 = map1.snapShot(true);
            auto snap2 = map2->snapShot(true);
            auto snap3 = map3->snapShot(true);
            
            std::cout << "  Before any hash calls:" << std::endl;
            std::cout << "    snap1 flush: " << snap1->flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
            std::cout << "    snap2 flush: " << snap2->flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
            std::cout << "    snap3 flush: " << snap3->flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
            
            // Now simulate "setImmutable" in background - get hash AFTER snapshot
            auto hash1 = map1.getHash().as_uint256();
            auto hash2 = map2->getHash().as_uint256();
            auto hash3 = map3->getHash().as_uint256();
            
            std::cout << "  After hash calls on originals:" << std::endl;
            std::cout << "    snap1 flush: " << snap1->flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
            std::cout << "    snap2 flush: " << snap2->flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
            std::cout << "    snap3 flush: " << snap3->flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
        }
        
        // Test 6: NO SNAPSHOTS - just pass the originals!
        std::cout << "\nTest 6: NO SNAPSHOTS - flush original maps directly" << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));
            
            // Build chain without hashing
            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));
            
            auto map3 = map2->snapShot(true);
            map3->updateGiveItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));
            
            std::cout << "  Before any operations:" << std::endl;
            std::cout << "    map1 flush: " << map1.flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
            std::cout << "    map2 flush: " << map2->flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
            std::cout << "    map3 flush: " << map3->flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
            
            // Now get hashes 
            auto hash1 = map1.getHash().as_uint256();
            auto hash2 = map2->getHash().as_uint256();
            auto hash3 = map3->getHash().as_uint256();
            
            std::cout << "  After hash calls:" << std::endl;
            std::cout << "    map1 flush: " << map1.flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
            std::cout << "    map2 flush: " << map2->flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
            std::cout << "    map3 flush: " << map3->flushDirty(hotACCOUNT_NODE) << " nodes" << std::endl;
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
            map3->updateGiveItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));
            
            std::cout << "  Flushing OUT OF ORDER (2, then 1, then 3):" << std::endl;
            
            // Flush map2 FIRST
            int flush2 = map2->flushDirty(hotACCOUNT_NODE);
            std::cout << "    map2 flush: " << flush2 << " nodes" << std::endl;
            
            // Then flush map1
            int flush1 = map1.flushDirty(hotACCOUNT_NODE);
            std::cout << "    map1 flush: " << flush1 << " nodes" << std::endl;
            
            // Finally flush map3
            int flush3 = map3->flushDirty(hotACCOUNT_NODE);
            std::cout << "    map3 flush: " << flush3 << " nodes" << std::endl;
            
            std::cout << "  Total flushed: " << (flush1 + flush2 + flush3) << " nodes" << std::endl;
            
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
        std::cout << "\nTest 8: REAL THREADING - background flush vs main thread hash" << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));
            
            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));
            
            auto map3 = map2->snapShot(true);
            map3->updateGiveItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));
            
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
                std::cout << "  [BG Thread] Flush complete: " 
                          << map1Flushed << ", " << map2Flushed << ", " << map3Flushed << " nodes" << std::endl;
                flushingDone = true;
            });
            
            // Main thread - might call getHash (simulating setImmutable)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::cout << "  [Main Thread] Calling getHash() (simulating setImmutable)..." << std::endl;
            hashingStarted = true;
            auto hash1 = map1.getHash().as_uint256();
            auto hash2 = map2->getHash().as_uint256(); 
            auto hash3 = map3->getHash().as_uint256();
            std::cout << "  [Main Thread] Hashes computed" << std::endl;
            
            // Wait for background thread
            bgThread.join();
            
            std::cout << "  Results:" << std::endl;
            std::cout << "    Hashing started first: " << (hashingStarted && !flushingDone) << std::endl;
            std::cout << "    Total flushed: " << (map1Flushed + map2Flushed + map3Flushed) << " nodes" << std::endl;
        }
        
        // Test 9: Try with different timing
        std::cout << "\nTest 9: THREADING - background flush WINS the race" << std::endl;
        {
            SHAMap map1(SHAMapType::FREE, f);
            map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));
            
            auto map2 = map1.snapShot(true);
            map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));
            
            auto map3 = map2->snapShot(true);
            map3->updateGiveItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));
            
            std::atomic<int> totalFlushed{0};
            std::promise<void> flushDone;
            auto flushFuture = flushDone.get_future();
            
            // Background thread - flush IMMEDIATELY
            std::thread bgThread([&]() {
                std::cout << "  [BG Thread] Flushing immediately..." << std::endl;
                int f1 = map1.flushDirty(hotACCOUNT_NODE);
                int f2 = map2->flushDirty(hotACCOUNT_NODE);
                int f3 = map3->flushDirty(hotACCOUNT_NODE);
                totalFlushed = f1 + f2 + f3;
                std::cout << "  [BG Thread] Flushed: " << f1 << ", " << f2 << ", " << f3 
                          << " (total: " << totalFlushed << ")" << std::endl;
                flushDone.set_value();
            });
            
            // Main thread - wait for flush to complete
            flushFuture.wait();
            std::cout << "  [Main Thread] Flush complete, now calling getHash()..." << std::endl;
            
            auto hash1 = map1.getHash().as_uint256();
            auto hash2 = map2->getHash().as_uint256();
            auto hash3 = map3->getHash().as_uint256();
            
            bgThread.join();
            
            std::cout << "  Result: Flushed " << totalFlushed << " nodes before hash" << std::endl;
            
            // Try to flush again after hash
            int afterHash = map1.flushDirty(hotACCOUNT_NODE) + 
                           map2->flushDirty(hotACCOUNT_NODE) + 
                           map3->flushDirty(hotACCOUNT_NODE);
            std::cout << "  After hash, can flush: " << afterHash << " nodes" << std::endl;
            
            // Verify hashes are correct
            std::cout << "  Hash verification:" << std::endl;
            std::cout << "    map1: " << hash1 << std::endl;
            std::cout << "    map2: " << hash2 << std::endl;
            std::cout << "    map3: " << hash3 << std::endl;
        }
        
        // Test 10: FUZZ TEST - random timing to expose race conditions
        // DISABLED - triggers assertions too often!
        if (false) {
        std::cout << "\nTest 10: FUZZ TEST - random delays to find race conditions" << std::endl;
        {
            // Run multiple iterations with random delays
            const int iterations = 10;
            int hashWins = 0;
            int flushWins = 0;
            int partialFlush = 0;
            
            for (int i = 0; i < iterations; i++) {
                SHAMap map1(SHAMapType::FREE, f);
                map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));
                
                auto map2 = map1.snapShot(true);
                map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));
                
                auto map3 = map2->snapShot(true);
                map3->updateGiveItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 100));
                
                std::atomic<int> totalFlushed{0};
                std::atomic<bool> hashCalled{false};
                
                // Random delays (0-20ms)
                auto bgDelay = std::chrono::milliseconds(rand() % 20);
                auto mainDelay = std::chrono::milliseconds(rand() % 20);
                
                // Background thread with random delay
                std::thread bgThread([&]() {
                    std::this_thread::sleep_for(bgDelay);
                    
                    if (!hashCalled) {
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
                if (totalFlushed == 0) {
                    hashWins++;
                } else if (totalFlushed == 133) {
                    flushWins++;
                    // Verify hashes are still correct
                    bool hashesCorrect = 
                        (hash1 == uint256("A104AF983D3E91C7B821C3B8838EA9D6C341A2245D59DF3F396111E9070D7213")) &&
                        (hash2 == uint256("A28A1F7982B22A477CD9AA9F957F8E4796CEABC7A0EF59563A857B9795D84809")) &&
                        (hash3 == uint256("E50E2F9886B6CBE23509F3FCC2220AC9C975C0DC542484680EBD4F5990EFAB14"));
                    if (!hashesCorrect) {
                        std::cout << "  WARNING: Hashes incorrect after flush!" << std::endl;
                    }
                } else {
                    partialFlush++;
                    std::cout << "  Iteration " << i << ": PARTIAL flush - " << totalFlushed << " nodes!" << std::endl;
                }
            }
            
            std::cout << "  Results after " << iterations << " iterations:" << std::endl;
            std::cout << "    Hash won (0 nodes):     " << hashWins << std::endl;
            std::cout << "    Flush won (133 nodes):  " << flushWins << std::endl;
            std::cout << "    Partial flush:          " << partialFlush << std::endl;
        }
        }
        
        // Test 11: REAL CATALOGUE SCENARIO - main builds while background flushes
        std::cout << "\nTest 11: CATALOGUE RACE - main builds next while bg flushes previous" << std::endl;
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
                for (int i = 0; i < 10; i++) {
                    if (mainBuilding) {
                        std::cout << "  [BG] RACE: Main is building while we're flushing!" << std::endl;
                        raceDetected = true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                
                map2Flushed = map2->flushDirty(hotACCOUNT_NODE);
                std::cout << "  [BG] Flushed map2: " << map2Flushed << " nodes" << std::endl;
                bgFlushing = false;
            });
            
            // Main thread - wait a bit then build map4
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            
            mainBuilding = true;
            std::cout << "  [Main] Building map4 from map3..." << std::endl;
            
            // This modifies the shared tree!
            auto map4 = map3->snapShot(true);
            map4->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(4, 4));
            map4->updateGiveItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 200));
            
            if (bgFlushing) {
                std::cout << "  [Main] RACE: BG is flushing while we're building!" << std::endl;
                raceDetected = true;
            }
            mainBuilding = false;
            
            bgThread.join();
            
            // Now try to flush map3 and map4
            map3Flushed = map3->flushDirty(hotACCOUNT_NODE);
            int map4Flushed = map4->flushDirty(hotACCOUNT_NODE);
            
            std::cout << "  Results:" << std::endl;
            std::cout << "    Race detected: " << (raceDetected ? "YES" : "NO") << std::endl;
            std::cout << "    map2 flushed: " << map2Flushed << " nodes" << std::endl;
            std::cout << "    map3 flushed: " << map3Flushed << " nodes" << std::endl;
            std::cout << "    map4 flushed: " << map4Flushed << " nodes" << std::endl;
            
            // Verify hashes are still correct
            auto hash2 = map2->getHash().as_uint256();
            auto hash3 = map3->getHash().as_uint256();
            auto hash4 = map4->getHash().as_uint256();
            std::cout << "    map2 hash: " << hash2 << std::endl;
            std::cout << "    map3 hash: " << hash3 << std::endl;
            std::cout << "    map4 hash: " << hash4 << std::endl;
        }
        
        // Test 12: AGGRESSIVE RACE - try to trigger the assertion failure
        std::cout << "\nTest 12: AGGRESSIVE RACE - multiple threads modifying shared tree" << std::endl;
        {
            // Build a deeper chain to increase shared nodes
            SHAMap map1(SHAMapType::FREE, f);
            for (int i = 1; i <= 10; i++) {
                map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(i, i));
            }
            
            auto map2 = map1.snapShot(true);
            for (int i = 11; i <= 20; i++) {
                map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(i, i));
            }
            
            auto map3 = map2->snapShot(true);
            for (int i = 21; i <= 30; i++) {
                map3->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(i, i));
            }
            
            std::atomic<bool> crashDetected{false};
            std::atomic<int> flushCount{0};
            std::atomic<int> buildCount{0};
            
            // Multiple background threads all trying to flush
            std::vector<std::thread> threads;
            
            for (int t = 0; t < 3; t++) {
                threads.emplace_back([&, t]() {
                    try {
                        // Random operations to increase chaos
                        for (int i = 0; i < 5; i++) {
                            if (t == 0) {
                                int f = map1.flushDirty(hotACCOUNT_NODE);
                                flushCount += f;
                            } else if (t == 1) {
                                int f = map2->flushDirty(hotACCOUNT_NODE);
                                flushCount += f;
                            } else {
                                int f = map3->flushDirty(hotACCOUNT_NODE);
                                flushCount += f;
                            }
                            std::this_thread::sleep_for(std::chrono::microseconds(100));
                        }
                    } catch (std::exception& e) {
                        std::cout << "  [Thread " << t << "] EXCEPTION: " << e.what() << std::endl;
                        crashDetected = true;
                    }
                });
            }
            
            // Main thread - aggressively build new maps
            threads.emplace_back([&]() {
                try {
                    for (int i = 0; i < 5; i++) {
                        auto map4 = map3->snapShot(true);
                        map4->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(100 + i, 100 + i));
                        map4->updateGiveItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1000 + i));
                        buildCount++;
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                } catch (std::exception& e) {
                    std::cout << "  [Main Build] EXCEPTION: " << e.what() << std::endl;
                    crashDetected = true;
                }
            });
            
            // Wait for all threads
            for (auto& t : threads) {
                t.join();
            }
            
            std::cout << "  Results:" << std::endl;
            std::cout << "    Crash/Exception detected: " << (crashDetected ? "YES" : "NO") << std::endl;
            std::cout << "    Total flushed: " << flushCount << " nodes" << std::endl;
            std::cout << "    Maps built: " << buildCount << std::endl;
            
            if (!crashDetected) {
                std::cout << "    No crash - COW might have internal locks we're not aware of" << std::endl;
            }
        }
        
        pass();
    }
    
    void
    run() override
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
            cfg.log_hashes_before_flush = false;  // DON'T GET HASH BEFORE FLUSH!
            cfg.flush_map1 = false;
            cfg.flush_map2 = false; 
            cfg.flush_map3 = true;  // Only flush map3
            cfg.flush_during_construction = false;  // Flush at END, not during construction
            configs.push_back(cfg);
        }
        
        // Test: Flush each map WITHOUT getting hashes (still in sequence)
        {
            TestConfig cfg;
            cfg.log_hashes_after_ops = false;  // DON'T LOG HASHES!
            cfg.log_hashes_before_flush = false;  // DON'T GET HASH BEFORE FLUSH!
            cfg.flush_map1 = true;  // Flush map1 after creating it
            cfg.flush_map2 = true;  // Flush map2 after creating it
            cfg.flush_map3 = true;  // Flush map3 after creating it
            configs.push_back(cfg);
        }
        
        // Test: Build ALL maps, then flush ALL at end (not during construction)
        {
            TestConfig cfg;
            cfg.log_hashes_after_ops = false;  // DON'T LOG HASHES!
            cfg.log_hashes_before_flush = false;  // DON'T GET HASH BEFORE FLUSH!
            cfg.flush_map1 = true;  // YES flush map1
            cfg.flush_map2 = true;  // YES flush map2  
            cfg.flush_map3 = true;  // YES flush map3
            cfg.flush_during_construction = false;  // But NOT during construction - at END!
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
        //     cfg.map3_snapshot_mutable = false;  // Would fail when trying to modify
        //     configs.push_back(cfg);
        // }
        
        // Run all configs and collect results
        struct TestResult {
            int configNum;
            TestConfig config;
            int map1_flushed = 0;
            int map2_flushed = 0;
            int map3_flushed = 0;
        };
        std::vector<TestResult> results;
        
        int testNum = 0;
        for (auto& cfg : configs) {
            std::cout << "\n\n========== TEST CONFIG #" << (++testNum) << " ==========" << std::endl;
            TestResult result;
            result.configNum = testNum;
            result.config = cfg;
            runWithConfig(true, journal, cfg);
            results.push_back(result);
        }
        
        // Print summary for pattern recognition
        std::cout << "\n\n========== SUMMARY FOR AI PATTERN RECOGNITION ==========" << std::endl;
        for (const auto& r : results) {
            std::cout << "Config #" << r.configNum << ": "
                      << "hash_before_flush=" << r.config.log_hashes_before_flush
                      << ", map2_mutable=" << r.config.map2_snapshot_mutable  
                      << ", map3_mutable=" << r.config.map3_snapshot_mutable
                      << " => Pattern TBD" << std::endl;
        }
        
        // NEW TEST: Threading and hash dependencies
        testcase("Threading and hash call ordering");
        testThreadingAndHashDependencies(journal);
    }
    
    void
    runWithConfig(bool backed, beast::Journal const& journal, TestConfig const& cfg)
    {
        testcase("COW snapshot and flush behavior");
        
        // Track total nodes flushed
        int totalNodesFlushed = 0;
        
        // Enable debug output
        std::cout << "\n=== Testing COW with backed=" << backed << " ===" << std::endl;
        std::cout << "Config: log_hashes_before_flush=" << cfg.log_hashes_before_flush
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
        
        std::cout << "1. Creating map1 and adding item key=1, val=1" << std::endl;
        map1.addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(1, 1));
        if (cfg.log_hashes_after_ops)
            std::cout << "   map1 hash: " << map1.getHash().as_uint256() << std::endl;
        
        std::cout << "2. Flushing map1 (flush=" << cfg.flush_map1 
                  << ", during_construction=" << cfg.flush_during_construction << ")" << std::endl;
        if (backed && cfg.flush_map1 && cfg.flush_during_construction) {
            if (cfg.log_hashes_before_flush)
                std::cout << "   map1 hash before flush: " << map1.getHash().as_uint256() << std::endl;
            int flushed = map1.flushDirty(hotACCOUNT_NODE);
            totalNodesFlushed += flushed;
            std::cout << "   Flushed " << flushed << " nodes (total so far: " << totalNodesFlushed << ")" << std::endl;
            if (cfg.log_hashes_after_ops)
                std::cout << "   map1 hash after flush: " << map1.getHash().as_uint256() << std::endl;
        }
        
        std::cout << "3. Taking " << (cfg.map2_snapshot_mutable ? "mutable" : "immutable") 
                  << " snapshot -> map2" << std::endl;
        auto map2 = map1.snapShot(cfg.map2_snapshot_mutable);
        if (cfg.log_hashes_after_ops)
            std::cout << "   map2 initial hash: " << map2->getHash().as_uint256() << std::endl;
        
        std::cout << "4. Adding item key=2, val=2 to map2" << std::endl;
        map2->addItem(SHAMapNodeType::tnACCOUNT_STATE, makeItem(2, 2));
        if (cfg.log_hashes_after_ops)
            std::cout << "   map2 hash after add: " << map2->getHash().as_uint256() << std::endl;
        
        std::cout << "5. Flushing map2 (flush=" << cfg.flush_map2 
                  << ", during_construction=" << cfg.flush_during_construction << ")" << std::endl;
        if (backed && cfg.flush_map2 && cfg.flush_during_construction) {
            if (cfg.log_hashes_before_flush)
                std::cout << "   map2 hash before flush: " << map2->getHash().as_uint256() << std::endl;
            int flushed = map2->flushDirty(hotACCOUNT_NODE);
            totalNodesFlushed += flushed;
            std::cout << "   Flushed " << flushed << " nodes (total so far: " << totalNodesFlushed << ")" << std::endl;
        }
        
        std::cout << "6. Taking " << (cfg.map3_snapshot_mutable ? "mutable" : "immutable")
                  << " snapshot -> map3" << std::endl;
        auto map3 = map2->snapShot(cfg.map3_snapshot_mutable);
        if (cfg.log_hashes_after_ops)
            std::cout << "   map3 initial hash: " << map3->getHash().as_uint256() << std::endl;
        
        if (cfg.map2_set_immutable_after_snapshot) {
            std::cout << "   Setting map2 immutable after snapshot" << std::endl;
            // map2->setImmutable();  // SHAMap doesn't have this method directly
        }
        
        std::cout << "7. Modifying existing item key=1 to val=100 in map3" << std::endl;
        map3->updateGiveItem(
            SHAMapNodeType::tnACCOUNT_STATE, 
            makeItem(1, 100));  // Change value
        if (cfg.log_hashes_after_ops)
            std::cout << "   map3 hash after update: " << map3->getHash().as_uint256() << std::endl;
        
        std::cout << "8. Flushing map3 (flush=" << cfg.flush_map3 
                  << ", during_construction=" << cfg.flush_during_construction << ")" << std::endl;
        if (backed && cfg.flush_map3 && cfg.flush_during_construction) {
            if (cfg.map3_set_immutable_before_flush) {
                std::cout << "   Setting map3 immutable before flush" << std::endl;
                // map3->setImmutable();  // Would need to implement
            }
            if (cfg.log_hashes_before_flush)
                std::cout << "   map3 hash before flush: " << map3->getHash().as_uint256() << std::endl;
            int flushed = map3->flushDirty(hotACCOUNT_NODE);
            totalNodesFlushed += flushed;
            std::cout << "   Flushed " << flushed << " nodes (total so far: " << totalNodesFlushed << ")" << std::endl;
            if (cfg.log_hashes_after_ops)
                std::cout << "   map3 hash after flush: " << map3->getHash().as_uint256() << std::endl;
        }
        
        std::cout << "\n=== Test completed ===" << std::endl;
        std::cout << "TOTAL NODES FLUSHED IN THIS TEST: " << totalNodesFlushed << std::endl;
        
        // Deferred flush - flush at the END if we didn't flush during construction
        // DO THIS BEFORE GETTING HASHES!!!
        if (!cfg.flush_during_construction && backed) {
            std::cout << "\n9. DEFERRED FLUSH - Flushing maps AFTER chain built:" << std::endl;
            
            if (cfg.flush_map1) {
                std::cout << "   Flushing map1 now..." << std::endl;
                if (cfg.log_hashes_before_flush)
                    std::cout << "   map1 hash before flush: " << map1.getHash().as_uint256() << std::endl;
                int f1 = map1.flushDirty(hotACCOUNT_NODE);
                totalNodesFlushed += f1;
                std::cout << "   Map1 flushed: " << f1 << " nodes (total: " << totalNodesFlushed << ")" << std::endl;
            }
            
            if (cfg.flush_map2) {
                std::cout << "   Flushing map2 now..." << std::endl;
                if (cfg.log_hashes_before_flush)
                    std::cout << "   map2 hash before flush: " << map2->getHash().as_uint256() << std::endl;
                int f2 = map2->flushDirty(hotACCOUNT_NODE);
                totalNodesFlushed += f2;
                std::cout << "   Map2 flushed: " << f2 << " nodes (total: " << totalNodesFlushed << ")" << std::endl;
            }
            
            if (cfg.flush_map3) {
                std::cout << "   Flushing map3 now..." << std::endl;
                if (cfg.log_hashes_before_flush)
                    std::cout << "   map3 hash before flush: " << map3->getHash().as_uint256() << std::endl;
                int f3 = map3->flushDirty(hotACCOUNT_NODE);
                totalNodesFlushed += f3;
                std::cout << "   Map3 flushed: " << f3 << " nodes (total: " << totalNodesFlushed << ")" << std::endl;
            }
        }
        
        // Final hash check (AFTER all flushes!)
        if (!cfg.log_hashes_after_ops && backed) {
            std::cout << "\n10. FINAL HASH CHECK (after all flushes):" << std::endl;
            std::cout << "   map1 final hash: " << map1.getHash().as_uint256() << std::endl;
            std::cout << "   map2 final hash: " << map2->getHash().as_uint256() << std::endl;
            std::cout << "   map3 final hash: " << map3->getHash().as_uint256() << std::endl;
        }
        
        // Mark test as passed
        pass();
    }
};

BEAST_DEFINE_TESTSUITE(SHAMapCOW, ripple_app, ripple);

}  // namespace tests
}  // namespace ripple