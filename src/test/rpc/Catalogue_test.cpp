//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2017 Ripple Labs Inc.

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
#include <ripple/beast/unit_test.h>
#include <ripple/protocol/jss.h>
#include <boost/filesystem.hpp>
#include <chrono>
#include <fstream>
#include <test/jtx.h>
#include <thread>

namespace ripple {

#pragma pack(push, 1)  // pack the struct tightly
struct TestCATLHeader
{
    uint32_t magic = 0x4C544143UL;
    uint32_t min_ledger;
    uint32_t max_ledger;
    uint16_t version;
    uint16_t network_id;
    uint64_t filesize = 0;  // Total size of the file including header
    std::array<uint8_t, 64> hash = {};  // SHA-512 hash, initially set to zeros
};
#pragma pack(pop)

class Catalogue_test : public beast::unit_test::suite
{
    // Helper to create test ledger data with complex state changes
    void
    prepareLedgerData(test::jtx::Env& env, int numLedgers)
    {
        using namespace test::jtx;
        Account alice{"alice"};
        Account bob{"bob"};
        Account charlie{"charlie"};

        env.fund(XRP(10000), alice, bob, charlie);
        env.close();

        // Set up trust lines and issue currency
        env(trust(bob, alice["USD"](1000)));
        env(trust(charlie, bob["EUR"](1000)));
        env.close();

        env(pay(alice, bob, alice["USD"](500)));
        env.close();

        // Create and remove an offer to test state deletion
        env(offer(bob, XRP(50), alice["USD"](1)));
        auto offerSeq =
            env.seq(bob) - 1;  // Get the sequence of the offer we just created
        env.close();

        // Cancel the offer
        env(offer_cancel(bob, offerSeq));
        env.close();

        // Create another offer with same account
        env(offer(bob, XRP(60), alice["USD"](2)));
        env.close();

        // Create a trust line and then remove it
        env(trust(charlie, bob["EUR"](1000)));
        env.close();
        env(trust(charlie, bob["EUR"](0)));
        env.close();

        // Recreate the same trust line
        env(trust(charlie, bob["EUR"](2000)));
        env.close();

        // Additional ledgers with various transactions
        for (int i = 0; i < numLedgers; ++i)
        {
            env(pay(alice, bob, XRP(100)));
            env(offer(bob, XRP(50), alice["USD"](1)));
            env.close();
        }
    }

    void
    testCatalogueCreateBadInput(FeatureBitset features)
    {
        testcase("catalogue_create: Invalid parameters");
        using namespace test::jtx;
        Env env{
            *this, envconfig(), features, nullptr, beast::severities::kInfo};

        // No parameters
        {
            auto const result =
                env.client().invoke("catalogue_create", {})[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Missing min_ledger
        {
            Json::Value params{Json::objectValue};
            params[jss::max_ledger] = 20;
            params[jss::output_file] = "/tmp/test.catl";
            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Missing max_ledger
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = 10;
            params[jss::output_file] = "/tmp/test.catl";
            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Missing output_file
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = 10;
            params[jss::max_ledger] = 20;
            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Invalid output path (not absolute)
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = 10;
            params[jss::max_ledger] = 20;
            params[jss::output_file] = "test.catl";
            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // min_ledger > max_ledger
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = 20;
            params[jss::max_ledger] = 10;
            params[jss::output_file] = "/tmp/test.catl";
            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }
    }

    void
    testCatalogueCreate(FeatureBitset features)
    {
        testcase("catalogue_create: Basic functionality");
        using namespace test::jtx;

        // Create environment and some test ledgers
        Env env{
            *this, envconfig(), features, nullptr, beast::severities::kInfo};
        prepareLedgerData(env, 5);

        boost::filesystem::path tempDir =
            boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path();
        boost::filesystem::create_directories(tempDir);

        auto cataloguePath = (tempDir / "test.catl").string();

        // Create catalogue
        Json::Value params{Json::objectValue};
        params[jss::min_ledger] = 3;
        params[jss::max_ledger] = 5;
        params[jss::output_file] = cataloguePath;

        auto const result =
            env.client().invoke("catalogue_create", params)[jss::result];

        BEAST_EXPECT(result[jss::status] == jss::success);
        BEAST_EXPECT(result[jss::min_ledger] == 3);
        BEAST_EXPECT(result[jss::max_ledger] == 5);
        BEAST_EXPECT(result[jss::output_file] == cataloguePath);
        BEAST_EXPECT(result[jss::file_size].asUInt() > 0);
        BEAST_EXPECT(result[jss::ledgers_written].asUInt() == 3);

        // Verify file exists and is not empty
        BEAST_EXPECT(boost::filesystem::exists(cataloguePath));
        BEAST_EXPECT(boost::filesystem::file_size(cataloguePath) > 0);

        boost::filesystem::remove_all(tempDir);
    }

    void
    testCatalogueLoadBadInput(FeatureBitset features)
    {
        testcase("catalogue_load: Invalid parameters");
        using namespace test::jtx;
        Env env{
            *this, envconfig(), features, nullptr, beast::severities::kInfo};

        // No parameters
        {
            auto const result =
                env.client().invoke("catalogue_load", {})[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Missing input_file
        {
            Json::Value params{Json::objectValue};
            auto const result =
                env.client().invoke("catalogue_load", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Invalid input path (not absolute)
        {
            Json::Value params{Json::objectValue};
            params[jss::input_file] = "test.catl";
            auto const result =
                env.client().invoke("catalogue_load", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Non-existent file
        {
            Json::Value params{Json::objectValue};
            params[jss::input_file] = "/tmp/nonexistent.catl";
            auto const result =
                env.client().invoke("catalogue_load", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "internal");
            BEAST_EXPECT(result[jss::status] == "error");
        }
    }

    void
    testCatalogueLoadAndVerify(FeatureBitset features)
    {
        testcase("catalogue_load: Load and verify");
        using namespace test::jtx;

        // Create environment and test data
        Env env{
            *this, envconfig(), features, nullptr, beast::severities::kInfo};
        prepareLedgerData(env, 5);

        // Store some key state information before catalogue creation
        auto const sourceLedger = env.closed();
        auto const bobKeylet = keylet::account(Account("bob").id());
        auto const charlieKeylet = keylet::account(Account("charlie").id());
        auto const eurTrustKeylet = keylet::line(
            Account("charlie").id(),
            Account("bob").id(),
            Currency(to_currency("EUR")));

        // Get original state entries
        auto const bobAcct = sourceLedger->read(bobKeylet);
        auto const charlieAcct = sourceLedger->read(charlieKeylet);
        auto const eurTrust = sourceLedger->read(eurTrustKeylet);

        BEAST_EXPECT(bobAcct != nullptr);
        BEAST_EXPECT(charlieAcct != nullptr);
        BEAST_EXPECT(eurTrust != nullptr);

        BEAST_EXPECT(
            eurTrust->getFieldAmount(sfLowLimit).mantissa() ==
            2000000000000000ULL);

        // Get initial complete_ledgers range
        auto const originalCompleteLedgers =
            env.app().getLedgerMaster().getCompleteLedgers();

        // Create temporary directory for test files
        boost::filesystem::path tempDir =
            boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path();
        boost::filesystem::create_directories(tempDir);

        auto cataloguePath = (tempDir / "test.catl").string();

        // First create a catalogue
        uint32_t minLedger = 3;
        uint32_t maxLedger = sourceLedger->info().seq;
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = minLedger;
            params[jss::max_ledger] = maxLedger;
            params[jss::output_file] = cataloguePath;

            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::status] == jss::success);
        }

        // Create a new environment for loading with unique port
        Env loadEnv{
            *this,
            test::jtx::envconfig(test::jtx::port_increment, 3),
            features,
            nullptr,
            beast::severities::kInfo};

        // Now load the catalogue
        Json::Value params{Json::objectValue};
        params[jss::input_file] = cataloguePath;

        auto const result =
            loadEnv.client().invoke("catalogue_load", params)[jss::result];

        BEAST_EXPECT(result[jss::status] == jss::success);
        BEAST_EXPECT(result[jss::ledger_min] == minLedger);
        BEAST_EXPECT(result[jss::ledger_max] == maxLedger);
        BEAST_EXPECT(result[jss::ledger_count] == (maxLedger - minLedger + 1));

        // Verify complete_ledgers reflects loaded ledgers
        auto const newCompleteLedgers =
            loadEnv.app().getLedgerMaster().getCompleteLedgers();

        BEAST_EXPECT(newCompleteLedgers == originalCompleteLedgers);

        // Verify the loaded state matches the original
        auto const loadedLedger = loadEnv.closed();

        // After loading each ledger

        // Compare all ledgers from 3 to 16 inclusive
        for (std::uint32_t seq = 3; seq <= 16; ++seq)
        {
            auto const sourceLedger =
                env.app().getLedgerMaster().getLedgerByHash(
                    env.app().getLedgerMaster().getHashBySeq(seq));

            auto const loadedLedger =
                loadEnv.app().getLedgerMaster().getLedgerByHash(
                    loadEnv.app().getLedgerMaster().getHashBySeq(seq));

            if (!sourceLedger || !loadedLedger)
            {
                BEAST_EXPECT(false);  // Test failure
                continue;
            }

            // Check basic ledger properties
            BEAST_EXPECT(sourceLedger->info().seq == loadedLedger->info().seq);
            BEAST_EXPECT(
                sourceLedger->info().hash == loadedLedger->info().hash);
            BEAST_EXPECT(
                sourceLedger->info().txHash == loadedLedger->info().txHash);
            BEAST_EXPECT(
                sourceLedger->info().accountHash ==
                loadedLedger->info().accountHash);
            BEAST_EXPECT(
                sourceLedger->info().parentHash ==
                loadedLedger->info().parentHash);
            BEAST_EXPECT(
                sourceLedger->info().drops == loadedLedger->info().drops);

            // Check time-related properties
            BEAST_EXPECT(
                sourceLedger->info().closeFlags ==
                loadedLedger->info().closeFlags);
            BEAST_EXPECT(
                sourceLedger->info().closeTimeResolution.count() ==
                loadedLedger->info().closeTimeResolution.count());
            BEAST_EXPECT(
                sourceLedger->info().closeTime.time_since_epoch().count() ==
                loadedLedger->info().closeTime.time_since_epoch().count());
            BEAST_EXPECT(
                sourceLedger->info()
                    .parentCloseTime.time_since_epoch()
                    .count() ==
                loadedLedger->info()
                    .parentCloseTime.time_since_epoch()
                    .count());

            // Check validation state
            BEAST_EXPECT(
                sourceLedger->info().validated ==
                loadedLedger->info().validated);
            BEAST_EXPECT(
                sourceLedger->info().accepted == loadedLedger->info().accepted);

            // Check SLE counts
            std::size_t sourceCount = 0;
            std::size_t loadedCount = 0;

            for (auto const& sle : sourceLedger->sles)
            {
                sourceCount++;
            }

            for (auto const& sle : loadedLedger->sles)
            {
                loadedCount++;
            }

            BEAST_EXPECT(sourceCount == loadedCount);

            // Check existence of imported keylets
            for (auto const& sle : sourceLedger->sles)
            {
                auto const key = sle->key();
                bool exists = loadedLedger->exists(keylet::unchecked(key));
                BEAST_EXPECT(exists);

                // If it exists, check the serialized form matches
                if (exists)
                {
                    auto loadedSle = loadedLedger->read(keylet::unchecked(key));
                    Serializer s1, s2;
                    sle->add(s1);
                    loadedSle->add(s2);
                    bool serializedEqual = (s1.peekData() == s2.peekData());
                    BEAST_EXPECT(serializedEqual);
                }
            }

            // Check for extra keys in loaded ledger that aren't in source
            for (auto const& sle : loadedLedger->sles)
            {
                auto const key = sle->key();
                BEAST_EXPECT(sourceLedger->exists(keylet::unchecked(key)));
            }
        }

        auto const loadedBobAcct = loadedLedger->read(bobKeylet);
        auto const loadedCharlieAcct = loadedLedger->read(charlieKeylet);
        auto const loadedEurTrust = loadedLedger->read(eurTrustKeylet);

        BEAST_EXPECT(!!loadedBobAcct);
        BEAST_EXPECT(!!loadedCharlieAcct);
        BEAST_EXPECT(!!loadedEurTrust);

        // Compare the serialized forms of the state objects
        bool const loaded =
            loadedBobAcct && loadedCharlieAcct && loadedEurTrust;

        Serializer s1, s2;
        if (loaded)
        {
            bobAcct->add(s1);
            loadedBobAcct->add(s2);
        }
        BEAST_EXPECT(loaded && s1.peekData() == s2.peekData());

        if (loaded)
        {
            s1.erase();
            s2.erase();
            charlieAcct->add(s1);
            loadedCharlieAcct->add(s2);
        }
        BEAST_EXPECT(loaded && s1.peekData() == s2.peekData());

        if (loaded)
        {
            s1.erase();
            s2.erase();
            eurTrust->add(s1);
            loadedEurTrust->add(s2);
        }

        BEAST_EXPECT(loaded && s1.peekData() == s2.peekData());

        // Verify trust line amount matches
        BEAST_EXPECT(
            loaded &&
            loadedEurTrust->getFieldAmount(sfLowLimit).mantissa() ==
                2000000000000000ULL);

        boost::filesystem::remove_all(tempDir);
    }

    void
    testNetworkMismatch(FeatureBitset features)
    {
        testcase("catalogue_load: Network ID mismatch");
        using namespace test::jtx;

        boost::filesystem::path tempDir =
            boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path();
        boost::filesystem::create_directories(tempDir);

        auto cataloguePath = (tempDir / "test.catl").string();

        // Create environment with different network IDs
        {
            Env env1{
                *this,
                envconfig([](std::unique_ptr<Config> cfg) {
                    cfg->NETWORK_ID = 123;
                    return cfg;
                }),
                features,
                nullptr,
                beast::severities::kInfo};
            prepareLedgerData(env1, 5);

            // Create catalogue with network ID 123
            {
                Json::Value params{Json::objectValue};
                params[jss::min_ledger] = 3;
                params[jss::max_ledger] = 5;
                params[jss::output_file] = cataloguePath;

                auto const result = env1.client().invoke(
                    "catalogue_create", params)[jss::result];
                BEAST_EXPECT(result[jss::status] == jss::success);
            }
        }

        {
            // Try to load catalogue in environment with different network ID
            Env env2{
                *this,
                envconfig([](std::unique_ptr<Config> cfg) {
                    cfg->NETWORK_ID = 456;
                    return cfg;
                }),
                features,
                nullptr,
                beast::severities::kInfo};

            {
                Json::Value params{Json::objectValue};
                params[jss::input_file] = cataloguePath;

                auto const result =
                    env2.client().invoke("catalogue_load", params)[jss::result];

                BEAST_EXPECT(result[jss::error] == "invalidParams");
                BEAST_EXPECT(result[jss::status] == "error");
            }
        }
        boost::filesystem::remove_all(tempDir);
    }

    void
    testCatalogueHashVerification(FeatureBitset features)
    {
        testcase("catalogue_load: Hash verification");
        using namespace test::jtx;

        // Create environment and test data
        Env env{
            *this, envconfig(), features, nullptr, beast::severities::kInfo};
        prepareLedgerData(env, 3);

        boost::filesystem::path tempDir =
            boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path();
        boost::filesystem::create_directories(tempDir);

        auto cataloguePath = (tempDir / "test.catl").string();

        // Create catalogue
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = 3;
            params[jss::max_ledger] = 5;
            params[jss::output_file] = cataloguePath;

            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::status] == jss::success);
            BEAST_EXPECT(result.isMember(jss::hash));
            std::string originalHash = result[jss::hash].asString();
            BEAST_EXPECT(!originalHash.empty());
        }

        // Test 1: Successful hash verification (normal load)
        {
            Json::Value params{Json::objectValue};
            params[jss::input_file] = cataloguePath;

            auto const result =
                env.client().invoke("catalogue_load", params)[jss::result];
            BEAST_EXPECT(result[jss::status] == jss::success);
            BEAST_EXPECT(result.isMember(jss::hash));
        }

        // Test 2: Corrupt the file and test hash mismatch detection
        {
            // Modify a byte in the middle of the file to cause hash mismatch
            std::fstream file(
                cataloguePath, std::ios::in | std::ios::out | std::ios::binary);
            BEAST_EXPECT(file.good());

            // Skip header and modify a byte
            file.seekp(sizeof(TestCATLHeader) + 100, std::ios::beg);
            char byte = 0xFF;
            file.write(&byte, 1);
            file.close();

            // Try to load the corrupted file
            Json::Value params{Json::objectValue};
            params[jss::input_file] = cataloguePath;

            auto const result =
                env.client().invoke("catalogue_load", params)[jss::result];
            BEAST_EXPECT(result[jss::status] == "error");
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(
                result[jss::error_message].asString().find(
                    "hash verification failed") != std::string::npos);
        }

        // Test 3: Test ignore_hash parameter
        {
            Json::Value params{Json::objectValue};
            params[jss::input_file] = cataloguePath;
            params[jss::ignore_hash] = true;

            auto const result =
                env.client().invoke("catalogue_load", params)[jss::result];
            // This might still fail due to data corruption, but not because of
            // hash verification The important part is that it didn't
            // immediately reject due to hash
            if (result[jss::status] == "error")
            {
                BEAST_EXPECT(
                    result[jss::error_message].asString().find(
                        "hash verification failed") == std::string::npos);
            }
        }

        boost::filesystem::remove_all(tempDir);
    }

    void
    testCatalogueFileSize(FeatureBitset features)
    {
        testcase("catalogue_load: File size verification");
        using namespace test::jtx;

        // Create environment and test data
        Env env{
            *this, envconfig(), features, nullptr, beast::severities::kInfo};
        prepareLedgerData(env, 3);

        boost::filesystem::path tempDir =
            boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path();
        boost::filesystem::create_directories(tempDir);

        auto cataloguePath = (tempDir / "test.catl").string();

        // Create catalogue
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = 3;
            params[jss::max_ledger] = 5;
            params[jss::output_file] = cataloguePath;

            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::status] == jss::success);
            BEAST_EXPECT(result.isMember(jss::file_size));
            uint64_t originalSize = result[jss::file_size].asUInt();
            BEAST_EXPECT(originalSize > 0);
        }

        // Test 1: Successful file size verification (normal load)
        {
            Json::Value params{Json::objectValue};
            params[jss::input_file] = cataloguePath;

            auto const result =
                env.client().invoke("catalogue_load", params)[jss::result];
            BEAST_EXPECT(result[jss::status] == jss::success);
            BEAST_EXPECT(result.isMember(jss::file_size));
        }

        // Test 2: Modify file size in header to cause mismatch
        {
            // Modify the filesize in the header to cause mismatch
            std::fstream file(
                cataloguePath, std::ios::in | std::ios::out | std::ios::binary);
            BEAST_EXPECT(file.good());

            file.seekp(offsetof(TestCATLHeader, filesize), std::ios::beg);
            uint64_t wrongSize = 12345;  // Some arbitrary wrong size
            file.write(
                reinterpret_cast<const char*>(&wrongSize), sizeof(wrongSize));
            file.close();

            // Try to load the modified file
            Json::Value params{Json::objectValue};
            params[jss::input_file] = cataloguePath;

            auto const result =
                env.client().invoke("catalogue_load", params)[jss::result];
            BEAST_EXPECT(result[jss::status] == "error");
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(
                result[jss::error_message].asString().find(
                    "file size mismatch") != std::string::npos);
        }

        boost::filesystem::remove_all(tempDir);
    }

    void
    testCatalogueCompression(FeatureBitset features)
    {
        testcase("catalogue: Compression levels");
        using namespace test::jtx;

        // Create environment and test data
        Env env{
            *this, envconfig(), features, nullptr, beast::severities::kInfo};
        prepareLedgerData(env, 5);

        boost::filesystem::path tempDir =
            boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path();
        boost::filesystem::create_directories(tempDir);

        std::vector<std::pair<std::string, Json::Value>> compressionTests = {
            {"no_compression", Json::Value(0)},       // Level 0 (none)
            {"min_compression", Json::Value(1)},      // Level 1 (minimal)
            {"default_compression", Json::Value(6)},  // Level 6 (default)
            {"max_compression", Json::Value(9)},      // Level 9 (maximum)
            {"boolean_true_compression",
             Json::Value(true)}  // Boolean true (should use default level 6)
        };

        uint64_t prevSize = 0;
        for (const auto& test : compressionTests)
        {
            std::string testName = test.first;
            Json::Value compressionLevel = test.second;

            auto cataloguePath = (tempDir / (testName + ".catl")).string();

            // Create catalogue with specific compression level
            Json::Value createParams{Json::objectValue};
            createParams[jss::min_ledger] = 3;
            createParams[jss::max_ledger] = 10;
            createParams[jss::output_file] = cataloguePath;
            createParams[jss::compression_level] = compressionLevel;

            auto createResult = env.client().invoke(
                "catalogue_create", createParams)[jss::result];

            BEAST_EXPECT(createResult[jss::status] == jss::success);

            uint64_t fileSize = createResult[jss::file_size].asUInt();
            BEAST_EXPECT(fileSize > 0);

            // Load the catalogue to verify it works
            Json::Value loadParams{Json::objectValue};
            loadParams[jss::input_file] = cataloguePath;

            auto loadResult =
                env.client().invoke("catalogue_load", loadParams)[jss::result];
            BEAST_EXPECT(loadResult[jss::status] == jss::success);

            // For levels > 0, verify size is smaller than uncompressed (or at
            // least not larger)
            if (prevSize > 0 && compressionLevel.asUInt() > 0)
            {
                BEAST_EXPECT(fileSize <= prevSize);
            }

            // Store size for comparison with next level
            if (compressionLevel.asUInt() == 0)
            {
                prevSize = fileSize;
            }

            // Verify compression level in response
            if (compressionLevel.isBool() && compressionLevel.asBool())
            {
                BEAST_EXPECT(
                    createResult[jss::compression_level].asUInt() == 6);
            }
            else
            {
                BEAST_EXPECT(
                    createResult[jss::compression_level].asUInt() ==
                    compressionLevel.asUInt());
            }
        }

        boost::filesystem::remove_all(tempDir);
    }

    void
    testCatalogueStatus(FeatureBitset features)
    {
        testcase("catalogue_status: Status reporting");
        using namespace test::jtx;

        // Create environment
        Env env{
            *this, envconfig(), features, nullptr, beast::severities::kInfo};

        boost::filesystem::path tempDir =
            boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path();
        boost::filesystem::create_directories(tempDir);

        auto cataloguePath = (tempDir / "test.catl").string();

        // Test 1: Check status when no job is running
        {
            auto result = env.client().invoke(
                "catalogue_status", Json::objectValue)[jss::result];
            std::cout << to_string(result) << "\n";
            BEAST_EXPECT(result[jss::job_status] == "no_job_running");
        }

        // TODO: add a parallel job test here... if anyone feels thats actually
        // needed

        boost::filesystem::remove_all(tempDir);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testCatalogueCreateBadInput(all);
        testCatalogueCreate(all);
        testCatalogueLoadBadInput(all);
        testCatalogueLoadAndVerify(all);
        testNetworkMismatch(all);
        testCatalogueHashVerification(all);
        testCatalogueFileSize(all);
        testCatalogueCompression(all);
        testCatalogueStatus(all);
    }
};

BEAST_DEFINE_TESTSUITE(Catalogue, rpc, ripple);

}  // namespace ripple
