//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 Ripple Labs Inc.

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

#include <ripple/beast/unit_test.h>
#include <array>
#include <blake3.h>
#include <chrono>
#include <cstring>
#include <openssl/sha.h>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace ripple {

class blake3_test : public beast::unit_test::suite
{
public:
    void
    testBasicHashing()
    {
        testcase("Basic BLAKE3 hashing");
        
        // Test vector from BLAKE3 specification
        const std::string input = "abc";
        std::array<uint8_t, BLAKE3_OUT_LEN> output;
        
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        blake3_hasher_update(&hasher, input.data(), input.size());
        blake3_hasher_finalize(&hasher, output.data(), BLAKE3_OUT_LEN);
        
        // Expected hash for "abc" (verified with online BLAKE3 calculator)
        const std::array<uint8_t, BLAKE3_OUT_LEN> expected = {
            0x64, 0x37, 0xb3, 0xac, 0x38, 0x46, 0x51, 0x33,
            0xff, 0xb6, 0x3b, 0x75, 0x27, 0x3a, 0x8d, 0xb5,
            0x48, 0xc5, 0x58, 0x46, 0x5d, 0x79, 0xdb, 0x03,
            0xfd, 0x35, 0x9c, 0x6c, 0xd5, 0xbd, 0x9d, 0x85
        };
        
        BEAST_EXPECT(output == expected);
    }
    
    void
    testEmptyInput()
    {
        testcase("BLAKE3 empty input");
        
        std::array<uint8_t, BLAKE3_OUT_LEN> output;
        
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        blake3_hasher_finalize(&hasher, output.data(), BLAKE3_OUT_LEN);
        
        // Expected hash for empty input
        const std::array<uint8_t, BLAKE3_OUT_LEN> expected = {
            0xaf, 0x13, 0x49, 0xb9, 0xf5, 0xf9, 0xa1, 0xa6,
            0xa0, 0x40, 0x4d, 0xea, 0x36, 0xdc, 0xc9, 0x49,
            0x9b, 0xcb, 0x25, 0xc9, 0xad, 0xc1, 0x12, 0xb7,
            0xcc, 0x9a, 0x93, 0xca, 0xe4, 0x1f, 0x32, 0x62
        };
        
        BEAST_EXPECT(output == expected);
    }
    
    void
    testIncrementalHashing()
    {
        testcase("BLAKE3 incremental hashing");
        
        // Hash "Hello, World!" in chunks
        const std::string part1 = "Hello, ";
        const std::string part2 = "World!";
        std::array<uint8_t, BLAKE3_OUT_LEN> incremental_output;
        
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        blake3_hasher_update(&hasher, part1.data(), part1.size());
        blake3_hasher_update(&hasher, part2.data(), part2.size());
        blake3_hasher_finalize(&hasher, incremental_output.data(), BLAKE3_OUT_LEN);
        
        // Hash the same string all at once
        const std::string full = part1 + part2;
        std::array<uint8_t, BLAKE3_OUT_LEN> single_output;
        
        blake3_hasher hasher2;
        blake3_hasher_init(&hasher2);
        blake3_hasher_update(&hasher2, full.data(), full.size());
        blake3_hasher_finalize(&hasher2, single_output.data(), BLAKE3_OUT_LEN);
        
        // Both methods should produce the same hash
        BEAST_EXPECT(incremental_output == single_output);
    }
    
    void
    testLargeInput()
    {
        testcase("BLAKE3 large input");
        
        // Create a large input (10KB)
        std::vector<uint8_t> large_input(10240);
        for (size_t i = 0; i < large_input.size(); ++i)
        {
            large_input[i] = static_cast<uint8_t>(i & 0xFF);
        }
        
        std::array<uint8_t, BLAKE3_OUT_LEN> output;
        
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        blake3_hasher_update(&hasher, large_input.data(), large_input.size());
        blake3_hasher_finalize(&hasher, output.data(), BLAKE3_OUT_LEN);
        
        // Just verify it doesn't crash and produces a hash
        // Check that output is not all zeros
        bool not_all_zeros = false;
        for (auto byte : output)
        {
            if (byte != 0)
            {
                not_all_zeros = true;
                break;
            }
        }
        BEAST_EXPECT(not_all_zeros);
    }
    
    void
    testVariableOutputLength()
    {
        testcase("BLAKE3 variable output length");
        
        const std::string input = "test";
        
        // Test different output lengths
        std::array<uint8_t, 16> output16;
        std::array<uint8_t, 32> output32;
        std::array<uint8_t, 64> output64;
        
        blake3_hasher hasher1;
        blake3_hasher_init(&hasher1);
        blake3_hasher_update(&hasher1, input.data(), input.size());
        blake3_hasher_finalize(&hasher1, output16.data(), 16);
        
        blake3_hasher hasher2;
        blake3_hasher_init(&hasher2);
        blake3_hasher_update(&hasher2, input.data(), input.size());
        blake3_hasher_finalize(&hasher2, output32.data(), 32);
        
        blake3_hasher hasher3;
        blake3_hasher_init(&hasher3);
        blake3_hasher_update(&hasher3, input.data(), input.size());
        blake3_hasher_finalize(&hasher3, output64.data(), 64);
        
        // First 16 bytes of 32-byte output should match 16-byte output
        BEAST_EXPECT(std::memcmp(output16.data(), output32.data(), 16) == 0);
        
        // First 32 bytes of 64-byte output should match 32-byte output
        BEAST_EXPECT(std::memcmp(output32.data(), output64.data(), 32) == 0);
    }
    
    void
    testKeyedMode()
    {
        testcase("BLAKE3 keyed mode");
        
        // 32-byte key
        uint8_t key[BLAKE3_KEY_LEN];
        std::memset(key, 0x42, BLAKE3_KEY_LEN);  // Fill with 0x42
        
        const std::string input = "message";
        std::array<uint8_t, BLAKE3_OUT_LEN> keyed_output;
        
        blake3_hasher hasher;
        blake3_hasher_init_keyed(&hasher, key);
        blake3_hasher_update(&hasher, input.data(), input.size());
        blake3_hasher_finalize(&hasher, keyed_output.data(), BLAKE3_OUT_LEN);
        
        // Hash without key should produce different output
        std::array<uint8_t, BLAKE3_OUT_LEN> unkeyed_output;
        blake3_hasher hasher2;
        blake3_hasher_init(&hasher2);
        blake3_hasher_update(&hasher2, input.data(), input.size());
        blake3_hasher_finalize(&hasher2, unkeyed_output.data(), BLAKE3_OUT_LEN);
        
        BEAST_EXPECT(keyed_output != unkeyed_output);
    }
    
    void
    testDerivationMode()
    {
        testcase("BLAKE3 key derivation mode");
        
        const char* context = "ripple 2024 key derivation";
        const std::string input = "input key material";
        std::array<uint8_t, BLAKE3_OUT_LEN> derived_key;
        
        blake3_hasher hasher;
        blake3_hasher_init_derive_key(&hasher, context);
        blake3_hasher_update(&hasher, input.data(), input.size());
        blake3_hasher_finalize(&hasher, derived_key.data(), BLAKE3_OUT_LEN);
        
        // Different context should produce different output
        const char* context2 = "different context";
        std::array<uint8_t, BLAKE3_OUT_LEN> derived_key2;
        
        blake3_hasher hasher2;
        blake3_hasher_init_derive_key(&hasher2, context2);
        blake3_hasher_update(&hasher2, input.data(), input.size());
        blake3_hasher_finalize(&hasher2, derived_key2.data(), BLAKE3_OUT_LEN);
        
        BEAST_EXPECT(derived_key != derived_key2);
    }

    void
    benchmarkKeyletDistribution()
    {
        testcase("Keylet Distribution Benchmark");

        // Real keylet distribution from your data (excluding 2-byte cached
        // ones)
        struct KeyletType
        {
            const char* name;
            size_t size;
            size_t count;
            double ratio;  // proportion of total operations
        };

        // Total non-cached keylet operations: ~189k
        // We'll scale to 626k total to match leaf count
        const size_t TOTAL_OPS = 626326;
        const size_t NON_CACHED_OPS = 188317;  // sum of all non-2-byte keylets

        std::vector<KeyletType> keylets = {
            {"ACCOUNT", 22, 76478, 76478.0 / NON_CACHED_OPS},
            {"HOOK", 22, 41740, 41740.0 / NON_CACHED_OPS},
            {"OWNER_DIR", 22, 3719, 3719.0 / NON_CACHED_OPS},
            {"HOOK_DEFINITION", 34, 17587, 17587.0 / NON_CACHED_OPS},
            {"DIR_PAGE", 42, 62, 62.0 / NON_CACHED_OPS},
            {"HOOK_STATE_DIR", 54, 19939, 19939.0 / NON_CACHED_OPS},
            {"TRUSTLINE", 62, 11882, 11882.0 / NON_CACHED_OPS},
            {"HOOK_STATE", 86, 17100, 17100.0 / NON_CACHED_OPS},
            {"URI_TOKEN", 102, 53, 53.0 / NON_CACHED_OPS}};

        // Pre-allocate random data for each size category
        std::unordered_map<size_t, std::vector<std::vector<uint8_t>>> testData;
        std::mt19937 rng(42);  // Deterministic seed for reproducibility
        std::uniform_int_distribution<uint8_t> dist(0, 255);

        for (const auto& keylet : keylets)
        {
            size_t scaledCount = static_cast<size_t>(keylet.ratio * TOTAL_OPS);
            testData[keylet.size].reserve(scaledCount);

            for (size_t i = 0; i < scaledCount; ++i)
            {
                std::vector<uint8_t> data(keylet.size);
                for (auto& byte : data)
                {
                    byte = dist(rng);
                }
                testData[keylet.size].push_back(std::move(data));
            }
        }

        // Count total test vectors
        size_t totalVectors = 0;
        for (const auto& [size, vectors] : testData)
        {
            totalVectors += vectors.size();
        }

        log << "Generated " << totalVectors
            << " test vectors matching keylet distribution\n";

        // Benchmark BLAKE3
        auto blake3Start = std::chrono::high_resolution_clock::now();

        for (const auto& [size, vectors] : testData)
        {
            for (const auto& data : vectors)
            {
                blake3_hasher hasher;
                blake3_hasher_init(&hasher);
                blake3_hasher_update(&hasher, data.data(), data.size());

                uint8_t output[BLAKE3_OUT_LEN];
                blake3_hasher_finalize(&hasher, output, BLAKE3_OUT_LEN);
            }
        }

        auto blake3End = std::chrono::high_resolution_clock::now();
        auto blake3Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            blake3End - blake3Start)
                            .count();

        // Benchmark SHA512Half (simplified version for testing)
        auto sha512Start = std::chrono::high_resolution_clock::now();

        for (const auto& [size, vectors] : testData)
        {
            for (const auto& data : vectors)
            {
                // Using OpenSSL SHA512 as proxy (sha512Half would add
                // truncation)
                SHA512_CTX ctx;
                SHA512_Init(&ctx);
                SHA512_Update(&ctx, data.data(), data.size());

                uint8_t output[64];
                SHA512_Final(output, &ctx);
                // In real sha512Half, we'd truncate to 32 bytes here
            }
        }

        auto sha512End = std::chrono::high_resolution_clock::now();
        auto sha512Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            sha512End - sha512Start)
                            .count();

        // Calculate weighted average input size
        double weightedAvgSize = 0;
        for (const auto& keylet : keylets)
        {
            weightedAvgSize += keylet.size * keylet.ratio;
        }

        // Report results
        log << "\n=== Keylet Distribution Benchmark Results ===\n";
        log << "Total operations: " << totalVectors << "\n";
        log << "Weighted average input size: " << weightedAvgSize << " bytes\n";
        log << "\nBLAKE3:\n";
        log << "  Total time: " << blake3Ns / 1000000.0 << " ms\n";
        log << "  Per hash: " << blake3Ns / totalVectors << " ns\n";
        log << "  Hashes/sec: " << (totalVectors * 1000000000.0) / blake3Ns
            << "\n";

        log << "\nSHA512:\n";
        log << "  Total time: " << sha512Ns / 1000000.0 << " ms\n";
        log << "  Per hash: " << sha512Ns / totalVectors << " ns\n";
        log << "  Hashes/sec: " << (totalVectors * 1000000000.0) / sha512Ns
            << "\n";

        log << "\nSpeedup: BLAKE3 is "
            << static_cast<double>(sha512Ns) / blake3Ns << "x faster\n";

        // Benchmark BLAKE3 with 512-byte buffer
        log << "\n=== 512-Byte Buffer Variants ===\n";

        auto blake3BufferStart = std::chrono::high_resolution_clock::now();

        for (const auto& [size, vectors] : testData)
        {
            for (const auto& data : vectors)
            {
                // Allocate 512-byte buffer each time
                alignas(64) uint8_t buffer[512];
                // Fast zero using memset (compiler optimizes to SIMD on Apple
                // Silicon)
                memset(buffer, 0, 512);
                // Copy actual data
                memcpy(buffer, data.data(), data.size());

                blake3_hasher hasher;
                blake3_hasher_init(&hasher);
                blake3_hasher_update(&hasher, buffer, 512);

                uint8_t output[BLAKE3_OUT_LEN];
                blake3_hasher_finalize(&hasher, output, BLAKE3_OUT_LEN);
            }
        }

        auto blake3BufferEnd = std::chrono::high_resolution_clock::now();
        auto blake3BufferNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                blake3BufferEnd - blake3BufferStart)
                .count();

        // Benchmark SHA512 with 512-byte buffer
        auto sha512BufferStart = std::chrono::high_resolution_clock::now();

        for (const auto& [size, vectors] : testData)
        {
            for (const auto& data : vectors)
            {
                // Allocate 512-byte buffer each time
                alignas(64) uint8_t buffer[512];
                // Fast zero using memset (compiler optimizes to SIMD on Apple
                // Silicon)
                memset(buffer, 0, 512);
                // Copy actual data
                memcpy(buffer, data.data(), data.size());

                SHA512_CTX ctx;
                SHA512_Init(&ctx);
                SHA512_Update(&ctx, buffer, 512);

                uint8_t output[64];
                SHA512_Final(output, &ctx);
            }
        }

        auto sha512BufferEnd = std::chrono::high_resolution_clock::now();
        auto sha512BufferNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                sha512BufferEnd - sha512BufferStart)
                .count();

        log << "\nBLAKE3 with 512-byte buffer:\n";
        log << "  Total time: " << blake3BufferNs / 1000000.0 << " ms\n";
        log << "  Per hash: " << blake3BufferNs / totalVectors << " ns\n";
        log << "  Hashes/sec: "
            << (totalVectors * 1000000000.0) / blake3BufferNs << "\n";
        log << "  Overhead vs normal: "
            << (static_cast<double>(blake3BufferNs) / blake3Ns - 1.0) * 100
            << "%\n";

        log << "\nSHA512 with 512-byte buffer:\n";
        log << "  Total time: " << sha512BufferNs / 1000000.0 << " ms\n";
        log << "  Per hash: " << sha512BufferNs / totalVectors << " ns\n";
        log << "  Hashes/sec: "
            << (totalVectors * 1000000000.0) / sha512BufferNs << "\n";
        log << "  Overhead vs normal: "
            << (static_cast<double>(sha512BufferNs) / sha512Ns - 1.0) * 100
            << "%\n";

        log << "\nFixed buffer speedup: BLAKE3 is "
            << static_cast<double>(sha512BufferNs) / blake3BufferNs
            << "x faster\n";

        // Verify BLAKE3 is faster
        BEAST_EXPECT(blake3Ns < sha512Ns);
    }

    void
    benchmarkInnerNodes()
    {
        testcase("Inner Node (516 bytes) Benchmark");

        const size_t INNER_NODE_SIZE =
            516;  // 4-byte prefix + 16 * 32-byte hashes
        const size_t INNER_NODE_COUNT = 211364;  // From your data

        // Pre-allocate test data
        std::vector<std::vector<uint8_t>> innerNodes;
        innerNodes.reserve(INNER_NODE_COUNT);

        std::mt19937 rng(42);
        std::uniform_int_distribution<uint8_t> dist(0, 255);

        for (size_t i = 0; i < INNER_NODE_COUNT; ++i)
        {
            std::vector<uint8_t> node(INNER_NODE_SIZE);
            for (auto& byte : node)
            {
                byte = dist(rng);
            }
            innerNodes.push_back(std::move(node));
        }

        log << "Generated " << INNER_NODE_COUNT << " inner nodes of "
            << INNER_NODE_SIZE << " bytes each\n";
        log << "Total data: "
            << (INNER_NODE_COUNT * INNER_NODE_SIZE) / (1024.0 * 1024.0)
            << " MB\n\n";

        // Benchmark BLAKE3
        auto blake3Start = std::chrono::high_resolution_clock::now();

        for (const auto& node : innerNodes)
        {
            blake3_hasher hasher;
            blake3_hasher_init(&hasher);
            blake3_hasher_update(&hasher, node.data(), node.size());

            uint8_t output[BLAKE3_OUT_LEN];
            blake3_hasher_finalize(&hasher, output, BLAKE3_OUT_LEN);
        }

        auto blake3End = std::chrono::high_resolution_clock::now();
        auto blake3Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            blake3End - blake3Start)
                            .count();

        // Benchmark SHA512
        auto sha512Start = std::chrono::high_resolution_clock::now();

        for (const auto& node : innerNodes)
        {
            SHA512_CTX ctx;
            SHA512_Init(&ctx);
            SHA512_Update(&ctx, node.data(), node.size());

            uint8_t output[64];
            SHA512_Final(output, &ctx);
        }

        auto sha512End = std::chrono::high_resolution_clock::now();
        auto sha512Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            sha512End - sha512Start)
                            .count();

        // Calculate throughput
        double totalMB =
            (INNER_NODE_COUNT * INNER_NODE_SIZE) / (1024.0 * 1024.0);

        log << "=== Inner Node (516 bytes) Results ===\n";

        log << "\nBLAKE3:\n";
        log << "  Total time: " << blake3Ns / 1000000.0 << " ms\n";
        log << "  Per hash: " << blake3Ns / INNER_NODE_COUNT << " ns\n";
        log << "  Hashes/sec: " << (INNER_NODE_COUNT * 1000000000.0) / blake3Ns
            << "\n";
        log << "  Throughput: " << (totalMB * 1000) / (blake3Ns / 1000000.0)
            << " MB/s\n";

        log << "\nSHA512:\n";
        log << "  Total time: " << sha512Ns / 1000000.0 << " ms\n";
        log << "  Per hash: " << sha512Ns / INNER_NODE_COUNT << " ns\n";
        log << "  Hashes/sec: " << (INNER_NODE_COUNT * 1000000000.0) / sha512Ns
            << "\n";
        log << "  Throughput: " << (totalMB * 1000) / (sha512Ns / 1000000.0)
            << " MB/s\n";

        log << "\nSpeedup: BLAKE3 is "
            << static_cast<double>(sha512Ns) / blake3Ns << "x faster\n";

        // Verify BLAKE3 is faster
        BEAST_EXPECT(blake3Ns < sha512Ns);
    }

    void
    run() override
    {
        // Comment out other tests for focused benchmarking
        // testBasicHashing();
        // testEmptyInput();
        // testIncrementalHashing();
        // testLargeInput();
        // testVariableOutputLength();
        // testKeyedMode();
        // testDerivationMode();
        // benchmarkKeyletDistribution();
        benchmarkInnerNodes();
    }
};

BEAST_DEFINE_TESTSUITE(blake3, protocol, ripple);

}  // namespace ripple