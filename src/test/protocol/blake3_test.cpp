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
#include <blake3.h>
#include <array>
#include <cstring>
#include <string>
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
    run() override
    {
        testBasicHashing();
        testEmptyInput();
        testIncrementalHashing();
        testLargeInput();
        testVariableOutputLength();
        testKeyedMode();
        testDerivationMode();
    }
};

BEAST_DEFINE_TESTSUITE(blake3, protocol, ripple);

}  // namespace ripple