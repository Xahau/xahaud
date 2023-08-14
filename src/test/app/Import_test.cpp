//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPLF

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


#include <ripple/app/tx/impl/Import.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/ledger/Directory.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Import.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>
#include <test/app/Import_json.h>

#define BEAST_REQUIRE(x)     \
    {                        \
        BEAST_EXPECT(!!(x)); \
        if (!(x))            \
            return;          \
    }
namespace ripple {
namespace test {

class Import_test : public beast::unit_test::suite
{
    std::unique_ptr<Config>
    makeNetworkConfig(uint32_t networkID)
    {
        using namespace jtx;
        std::vector<std::string> const keys = {
            "ED74D4036C6591A4BDF9C54CEFA39B996A"
            "5DCE5F86D11FDA1874481CE9D5A1CDC1"};
        return envconfig([&](std::unique_ptr<Config> cfg) {
            cfg->section(SECTION_RPC_STARTUP)
                .append(
                    "{ \"command\": \"log_level\", \"severity\": \"warn\" "
                    "}");
            cfg->NETWORK_ID = networkID;
            Section config;
            config.append(
                {"reference_fee = 50",
                 "account_reserve = 1000000",
                 "owner_reserve = 200000"});
            auto setup = setup_FeeVote(config);
            cfg->FEES = setup;

            for (auto const& strPk : keys)
            {
                auto pkHex = strUnHex(strPk);
                if (!pkHex)
                    Throw<std::runtime_error>(
                        "Import VL Key '" + strPk + "' was not valid hex.");

                auto const pkType = publicKeyType(makeSlice(*pkHex));
                if (!pkType)
                    Throw<std::runtime_error>(
                        "Import VL Key '" + strPk +
                        "' was not a valid key type.");

                cfg->IMPORT_VL_KEYS.emplace(strPk, makeSlice(*pkHex));
            }
            return cfg;
        });
    }

    static Json::Value
    import(jtx::Account const& account, Json::Value const& xpop)
    {
        using namespace jtx;
        Json::Value jv;
        std::string strJson = Json::FastWriter().write(xpop);
        jv[jss::TransactionType] = jss::Import;
        jv[jss::Account] = account.human();
        jv[sfBlob.jsonName] = strHex(strJson);
        return jv;
    }

    static std::pair<uint256, std::shared_ptr<SLE const>>
    accountKeyAndSle(ReadView const& view, jtx::Account const& account)
    {
        auto const k = keylet::account(account);
        return {k.key, view.read(k)};
    }

    static std::pair<uint256, std::shared_ptr<SLE const>>
    signersKeyAndSle(ReadView const& view, jtx::Account const& account)
    {
        auto const k = keylet::signers(account);
        return {k.key, view.read(k)};
    }

    static std::size_t
    ownerDirCount(ReadView const& view, jtx::Account const& acct)
    {
        ripple::Dir const ownerDir(view, keylet::ownerDir(acct.id()));
        return std::distance(ownerDir.begin(), ownerDir.end());
    };

    static Json::Value
    loadXpop(std::string content)
    {
        // If the string is empty, return an empty Json::Value
        if (content.empty())
        {
            std::cout << "JSON string was empty" << "\n";
            return {};
        }

        Json::Value jsonValue;
        Json::Reader reader;
        reader.parse(content, jsonValue);
        return jsonValue;
    }

    void
    testComputeStartingBalance(FeatureBitset features)
    {
        testcase("import header - computeStartingBonus");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(11111)};
        
        // old fee
        XRPAmount const value = Import::computeStartingBonus(*env.current());
        BEAST_EXPECT(value == drops(2000000));

        // todo: new fee
    }

    void
    testIsHex(FeatureBitset features)
    {
        testcase("import utils - isHex");
        bool const result = isHex("DEADBEEF");
        BEAST_EXPECT(result == true);
        bool const result1 = isHex("Hello world");
        BEAST_EXPECT(result1 == false);
    }

    void
    testIsBase58(FeatureBitset features)
    {
        testcase("import utils - isBase58");
        bool const result = isBase58("hE45rTtDcGvFz");
        BEAST_EXPECT(result == true);
        bool const result1 = isBase58("Hello world");
        BEAST_EXPECT(result1 == false);
    }

    void
    testIsBase64(FeatureBitset features)
    {
        testcase("import utils - isBase64");
        bool const result = isBase64("SGVsbG8gV29ybGQh");
        BEAST_EXPECT(result == true);
        bool const result1 = isBase64("Hello world");
        BEAST_EXPECT(result1 == false);
    }

    void
    testParseUint64(FeatureBitset features)
    {
        testcase("import utils - parseUint64");
        std::optional<uint64_t> result1 = parse_uint64("1234");
        BEAST_EXPECT(result1 == 1234);

        // std::optional<uint64_t> result2 = parse_uint64("0xFFAABBCCDD22");
        // std::cout << "RESULT: " << *result2 << "\n";
        // BEAST_EXPECT(result2 == 0xFFAABBCCDD22);

        std::optional<uint64_t> result3 = parse_uint64("2147483647");
        BEAST_EXPECT(result3 == 2147483647);

        std::optional<uint64_t> result4 = parse_uint64("18446744073709551615");
        BEAST_EXPECT(result4 == 18446744073709551615ull);
    }

    void
    testSyntaxCheckProofArray(FeatureBitset features)
    {
    }

    void
    testSyntaxCheckProofObject(FeatureBitset features)
    {
        testcase("import utils - syntaxCheckProof: is object");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(11111)};

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        std::string strJson = R"json({
            "children": {
                "3": {
                    "children": {},
                    "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                    "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                }
            },
            "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
            "key": "0000000000000000000000000000000000000000000000000000000000000000"
        })json";

        Json::Value proof;
        Json::Reader reader;
        reader.parse(strJson, proof);

        // XPOP.transaction.proof list entry has wrong format
        {
            BEAST_EXPECT(syntaxCheckProof(proof, env.journal, 65) == false);
        }
        // XPOP.transaction.proof invalid branch size
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["30"] = tmpProof[jss::children]["3"];
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof child node was not 0-F
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["not a hex"] = tmpProof[jss::children]["3"];
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid child (must be object)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"] = "not object";
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid hash (must be string)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::hash] = 1234;
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid hash size (must be 64)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::hash] =
                "00000000000000000000000000000000000000000000000000000000000000"
                "000";
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid key (must be string)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::key] = 1234;
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid key size (must be 64)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::key] =
                "00000000000000000000000000000000000000000000000000000000000000"
                "000";
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid node children (must be object)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::children] = "bad object";
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof bad children format
        // invalid node child children (must be hex)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::children] =
                tmpProof[jss::children]["3"];
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // success
        {
            BEAST_EXPECT(syntaxCheckProof(proof, env.journal) == true);
        }
    }

    void
    testSyntaxCheckXPOP(FeatureBitset features)
    {
        testcase("import utils - syntaxCheckXPOP");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(11111)};

        // blob empty
        {
            Blob raw{};
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // blob string empty
        {
            Blob raw{0};
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP failed to parse string json
        {
            Blob raw{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP is not a JSON object
        {
            std::string strJson = R"json([])json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger is not a JSON object
        {
            std::string strJson = R"json({
                "ledger": "not object"
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.transaction is not a JSON object
        {
            std::string strJson = R"json({
                "ledger": {},
                "transaction": "not object"
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.validation is not a JSON object
        {
            std::string strJson = R"json({
                "ledger": {},
                "transaction": {},
                "validation": "not object"
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.LEDGER

        // XPOP.ledger.acroot missing or wrong format
        // invalid xpop.ledger.acroot (must be string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": {}
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.acroot missing or wrong format
        // invalid xpop.ledger.acroot (must be 64)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "00000000000000000000000000000000000000000000000000000000000000000"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.acroot missing or wrong format
        // invalid xpop.ledger.acroot (must be hex)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "this string will pass a length check but fail when checking hex."
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.txroot missing or wrong format
        // invalid xpop.ledger.txroot (must be string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": {}
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.txroot missing or wrong format
        // invalid xpop.ledger.txroot (must be 64)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "00000000000000000000000000000000000000000000000000000000000000000"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.txroot missing or wrong format
        // invalid xpop.ledger.txroot (must be hex)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "this string will pass a length check but fail when checking hex."
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.phash missing or wrong format
        // invalid xpop.ledger.phash (must be string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": {}
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.phash missing or wrong format
        // invalid xpop.ledger.phash (must be 64)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "00000000000000000000000000000000000000000000000000000000000000000"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.phash missing or wrong format
        // invalid xpop.ledger.phash (must be hex)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "this string will pass a length check but fail when checking hex."
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.close missing or wrong format
        // invalid xpop.ledger.close (must be int)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": "738786851"

                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.coins missing or wrong format
        // invalid xpop.ledger.coins (must be int or string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": {}
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.coins missing or wrong format
        // invalid xpop.ledger.coins (must be int or string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "not an int string"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.cres missing or wrong format
        // invalid xpop.ledger.cres (must be int)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": "not an int"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.flags missing or wrong format
        // invalid xpop.ledger.flags (must be int)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": "not an int"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.pclose missing or wrong format
        // invalid xpop.ledger.pclose (must be int)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": "not an int"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.TRANSACTION

        // XPOP.transaction.blob missing or wrong format
        // invalid xpop.transaction.blob (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": {}
                },
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.transaction.blob missing or wrong format
        // invalid xpop.transaction.blob (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "not a hex"
                },
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.transaction.meta missing or wrong format
        // invalid xpop.transaction.meta (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": {}
                },
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.transaction.meta missing or wrong format
        // invalid xpop.transaction.meta (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "not a hex"
                },
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.transaction.proof failed syntax check
        // invalid xpop.transaction.proof (must pass syntaxCheckProof)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {}
                },
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.data missing or wrong format
        // invalid xpop.validation.data (must be object)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": "not an object"
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl missing or wrong format
        // invalid xpop.validation.unl (must be object)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": "not an object"
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.TRANSACTION.DATA

        // XPOP.validation.data entry has wrong format
        // invalid xpop.validation.data key/value (must be base58 hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A"
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "not base 58": "",
                    },
                    "unl": {}
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.public_key missing or
        // invalid xpop.validation.data key/value (must be base58 hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": 1,
                    },
                    "unl": {}
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.public_key invalid key type.
        // invalid xpop.validation.data key/value (must be base58 hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "not a hex",
                    },
                    "unl": {}
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.TRANSACTION.UNL

        // XPOP.validation.unl.public_key missing or
        // invalid xpop.validation.unl.public_key (must be hex string with valid
        // key type)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": 1
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.public_key missing or
        // invalid xpop.validation.unl.public_key (must be hex string with valid
        // key type)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "not a hex"
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.public_key missing or
        // invalid xpop.validation.unl.public_key (must be hex string with valid
        // key type)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "0074D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1"
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.manifest missing or wrong
        // invalid xpop.validation.unl.manifest (must be string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": 1
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.blob missing or wrong
        // invalid xpop.validation.unl.blob (must be base 64 string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": 1
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.blob missing or wrong
        // invalid xpop.validation.unl.blob (must be base 64 string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": "not a base 64 string"
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.signature missing or wrong
        // invalid xpop.validation.unl.signature (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                        "signature": 1
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.signature missing or wrong
        // invalid xpop.validation.unl.signature (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                        "signature": "not a hex"
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.version missing or
        // invalid xpop.validation.unl.version (must be int)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                        "signature": "849F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1F70D",
                        "version": "not an int"
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl entry has wrong format

        // valid xpop
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                        "signature": "849F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1F70D",
                        "version": 1
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(syntaxCheckXPOP(raw, env.journal).has_value() == true);
        }
    }

    void
    testGetVLInfo(FeatureBitset features)
    {
        testcase("import utils - getVLInfo");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(11111)};

        std::string strJson = R"json({
            "ledger": {
                "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                "close": 738786851,
                "coins": "99999998999999868",
                "cres": 10,
                "flags": 10,
                "pclose": 738786851
            },
            "transaction": {
                "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                "proof": {
                    "children": {
                        "3": {
                        "children": {},
                        "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                        "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                        }
                    },
                    "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "key": "0000000000000000000000000000000000000000000000000000000000000000"
                }
            },
            "validation": {
                "data": {
                    "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                },
                "unl": {
                    "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                    "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                    "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                    "signature": "849F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1F70D",
                    "version": 1
                }
            }
        })json";

        Json::Value xpop;
        Json::Reader reader;
        reader.parse(strJson, xpop);
        Json::FastWriter writer;

        // Import: unl blob was not valid json (after base64 decoding)
        {
            Json::Value tmpXpop = xpop;
            tmpXpop[jss::validation][jss::unl][jss::blob] = "YmFkSnNvbg==";
            std::string strJson = writer.write(tmpXpop);
            Blob raw(strJson.begin(), strJson.end());
            auto const xpop = syntaxCheckXPOP(raw, env.journal);
            BEAST_EXPECT(getVLInfo(*xpop, env.journal).has_value() == false);
        }

        // Import: failed to deserialize manifest
        {
            Json::Value tmpXpop = xpop;
            tmpXpop[jss::validation][jss::unl][jss::manifest] = "YmFkSnNvbg==";
            std::string strJson = writer.write(tmpXpop);
            Blob raw(strJson.begin(), strJson.end());
            auto const xpop = syntaxCheckXPOP(raw, env.journal);
            BEAST_EXPECT(getVLInfo(*xpop, env.journal).has_value() == false);
        }

        // Import: valid unl blob
        {
            Json::Value tmpXpop = xpop;
            std::string strJson = writer.write(tmpXpop);
            Blob raw(strJson.begin(), strJson.end());
            auto const xpop = syntaxCheckXPOP(raw, env.journal);
            auto const [seq, masterKey] = *getVLInfo(*xpop, env.journal);
            BEAST_EXPECT(std::to_string(seq) == "2");
            BEAST_EXPECT(
                strHex(masterKey) ==
                "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
                "CDC1");
        }
    }

    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        for (bool const withImport : {false, true})
        {
            // If the Import amendment is not enabled, you should not be able
            // to import.
            auto const amend = withImport ? features : features - featureImport;
            Env env{*this, makeNetworkConfig(21337), amend};

            // setup env
            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            auto const txResult =
                withImport ? ter(tesSUCCESS) : ter(temDISABLED);
            auto const ownerDir = withImport ? 1 : 0;

            // IMPORT - Account Set
            env(import(alice, loadXpop(ImportTCAccountSet::w_seed)), txResult);
            env.close();

        }
    }

    void
    testInvalidPreflight(FeatureBitset features)
    {
        testcase("import invalid preflight");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");

        env.fund(XRP(1000), alice, bob);
        env.close();

        //----------------------------------------------------------------------
        // preflight

        // temDISABLED - disabled - CHECKED IN ENABLED
        // return ret; - preflight1 no success

        // temMALFORMED - sfFee cannot be 0
        {
            Json::Value tx = import(alice, loadXpop(ImportTCAccountSet::w_seed));
            STAmount const& fee = XRP(10);
            tx[jss::Fee] = fee.getJson(JsonOptions::none);
            env(tx, ter(temMALFORMED));
        }

        //* DA: Technically breaking the size throws before preflight
        // temMALFORMED
        // temMALFORMED - Import: blob was more than 512kib
        // {
        //     ripple::Blob blob;
        //     blob.resize(513 * 1024);
        //     env(import(alice, blob), ter(temMALFORMED));
        // }

        // temMALFORMED - sfAmount field must be in drops
        {
            Json::Value tx = import(alice, loadXpop(ImportTCAccountSet::w_seed));
            STAmount const& amount = XRP(-1);
            tx[jss::Amount] = amount.getJson(JsonOptions::none);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - !xpop | XPOP.validation is not a JSON object
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation] = {};  // one of many ways to throw error
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation.unl.public_key was not valid hex
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::public_key] = "not a hex";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation.unl.public_key was not a recognised
        // public key type
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::public_key] =
                "0084D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
                "CDC1";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // getInnerTxn - !xpop
        // DA: Duplicate -

        // getInnerTxn - failed to deserialize tx blob inside xpop (invalid hex)
        // DA: Duplicate - "XPOP.transaction.blob missing or wrong format"

        // getInnerTxn - failed to deserialize tx meta inside xpop (invalid hex)
        // DA: Duplicate - "XPOP.transaction.meta missing or wrong format"

        // getInnerTxn - failed to deserialize tx blob/meta inside xpop
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] = "DEADBEEF";
            tmpXpop[jss::transaction][jss::meta] = "DEADBEEF";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // // temMALFORMED - !stpTrans
        // // DA: Duplicate - getInnerTxn (Any Failure)

        // temMALFORMED - Import: attempted to import xpop containing an emitted
        // or pseudo txn.
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "12000322000000002400000002201B00000069201D0000535968400000003B"
                "9ACA0073210388935426E0D08083314842EDFBB2D517BD47699F9A4527318A"
                "8E10468C97C0527446304402200E40CA821A7BFE347448FFA6540F67C7FFBF"
                "0287756D324DFBADCEDE2B23782C02207BE1B1294F1A1D5AC21990740B78F9"
                "C0693431237D6E07FE84228082986E50FF8114AE123A8556F3CF9115471137"
                "6AFB0F894F832B3DED202E000000013D00000000000000015B2200676F4B9B"
                "45C5ADE9DE3C8CD01200CB12DFF8C792220DB088FE615BE5C2905C207064D8"
                "1954F6A7225A8BAADF5A3042016BFB87355D1D0AFEDBAA8FB22F98355D745F"
                "398EEE9E6B294BBE6A5681A31A6107243D19384E277B5A7B1F23B8C83DE78A"
                "14AE123A8556F3CF91154711376AFB0F894F832B3DE1";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: inner txn lacked transaction result
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::meta] =
                "201C00000006F8E5110061250000005655463E39A6AFDDA77DBF3591BF3C2A"
                "4BE9BB8D9113BF6D0797EB403C3D0D894FEF5692FA6A9FC8EA6018D5D16532"
                "D7795C91BFB0831355BDFDA177E86C8BF997985FE624000000026240000000"
                "773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481"
                "14AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F1";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: inner txn did not have a tesSUCCESS or tec
        // result
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::meta] =
                "201C00000006F8E5110061250000005655463E39A6AFDDA77DBF3591BF3C2A"
                "4BE9BB8D9113BF6D0797EB403C3D0D894FEF5692FA6A9FC8EA6018D5D16532"
                "D7795C91BFB0831355BDFDA177E86C8BF997985FE624000000026240000000"
                "773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481"
                "14AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F103103C";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: import and txn inside xpop must be signed by
        // the same account
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import(bob, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: attempted to import xpop containing a txn with
        // a sfNetworkID field.
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "120003210000535922000000002400000002201B00000069201D0000535968"
                "400000003B9ACA0073210388935426E0D08083314842EDFBB2D517BD47699F"
                "9A4527318A8E10468C97C0527446304402200E40CA821A7BFE347448FFA654"
                "0F67C7FFBF0287756D324DFBADCEDE2B23782C02207BE1B1294F1A1D5AC219"
                "90740B78F9C0693431237D6E07FE84228082986E50FF8114AE123A8556F3CF"
                "91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: OperationLimit missing from inner xpop txn.
        // outer txid:
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "12000322000000002400000002201B0000006C68400000003B9ACA007321ED"
                "A8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C"
                "747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F"
                "232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C51"
                "77C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: Wrong network ID for OperationLimit in inner
        // txn. outer txid:
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "12000322000000002400000002201B0000006C201D0000535A68400000003B"
                "9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62"
                "ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593EC"
                "C21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF"
                "3EC206AD3C5177C519560F8114AE123A8556F3CF91154711376AFB0F894F83"
                "2B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(telWRONG_NETWORK));
        }

        // temMALFORMED - Import: inner txn must be an AccountSet, SetRegularKey
        // or SignerListSet transaction.
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "12006322000000002400000002201B0000006C201D0000535968400000003B"
                "9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62"
                "ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593EC"
                "C21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF"
                "3EC206AD3C5177C519560F8114AE123A8556F3CF91154711376AFB0F894F83"
                "2B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: outer and inner txns were (multi) signed with
        // different keys.

        // temMALFORMED - Import: outer and inner txns were signed with
        // different keys.
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "12000322000000002400000002201B0000006C201D0000535968400000003B"
                "9ACA007321EBA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62"
                "ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593EC"
                "C21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF"
                "3EC206AD3C5177C519560F8114AE123A8556F3CF91154711376AFB0F894F83"
                "2B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: inner txn signature verify failed
        // DA: TODO: ASK FOR HELP

        // temMALFORMED - Import: failed to deserialize manifest on txid
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::manifest] = "YmFkSnNvbg==";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: manifest master key did not match top level
        // master key in unl section of xpop
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::manifest] =
                "JAAAAAFxIe2E1ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+"
                "b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338"
                "jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/"
                "ikfgf9SZOlOGcBcBJAw44PLjH+"
                "HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+"
                "IILJIiIKU/+1Uxx0FRpQbMDA==";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: manifest signature invalid
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::manifest] =
                "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+"
                "b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkA3UjfY5zOEkhq31tU4338"
                "jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/"
                "ikfgf9SZOlOGcBcBJAw34PLjH+"
                "HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+"
                "IILJIiIKU/+1Uxx0FRpQbMDA==";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob not signed correctly
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::signature] =
                "949F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578"
                "943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1"
                "F70D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob not signed correctly
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::signature] = "not a hex";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // DA: GOOD SIGNATURE NOT JSON
        // temMALFORMED - Import: unl blob was not valid json (after base64
        // decoding)
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] = "YmFkSnNvbg==";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // DA: GOOD SIGNATURE GOOD JSON MISSING FIELDS
        // temMALFORMED - Import: unl blob json (after base64 decoding) lacked
        // required fields and/or types

        // temMALFORMED - Import: unl blob validUntil <= validFrom
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MSwiZWZmZWN0aXZlIjowLCJleHBpcmF0aW9uIjowLCJ2YW"
                "xpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVB"
                "RkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMT"
                "FDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlE"
                "TUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S2"
                "8raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2"
                "TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWG"
                "dTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lO"
                "U0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcD"
                "dWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlv"
                "bl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGRE"
                "U5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3Qi"
                "OiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeH"
                "RIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VG"
                "clU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3Tj"
                "Zpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQx"
                "QVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RT"
                "lTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9j"
                "eWNyRi9DdFNBND0ifV19";
            tmpXpop[jss::validation][jss::unl][jss::signature] =
                "2B3C0ECB63C82454522188337354C480693A9BCD64E776B4DBAD4C61B9E72D"
                "D4CC1DC237B06891E57C623C38506FE8E01B1914C9413471BCC160111E2829"
                "7606";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob expired
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MSwiZWZmZWN0aXZlIjowLCJleHBpcmF0aW9uIjoxLCJ2YW"
                "xpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVB"
                "RkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMT"
                "FDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlE"
                "TUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S2"
                "8raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2"
                "TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWG"
                "dTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lO"
                "U0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcD"
                "dWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlv"
                "bl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGRE"
                "U5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3Qi"
                "OiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeH"
                "RIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VG"
                "clU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3Tj"
                "Zpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQx"
                "QVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RT"
                "lTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9j"
                "eWNyRi9DdFNBND0ifV19";
            tmpXpop[jss::validation][jss::unl][jss::signature] =
                "FA82662A23EC78E9644C65F752B7A58F61F35AC36C260F9E9D5CAC7D53D16D"
                "5D615A02A6462F2618C162D089AD2E3BA7D656728392180517A81B4C47F86A"
                "640D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob not yet valid
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MSwiZWZmZWN0aXZlIjozNjAwLCJleHBpcmF0aW9uIjo4Nj"
                "QwMCwidmFsaWRhdG9ycyI6W3sidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRUQz"
                "OEJENDQ1QUZENjIxNTk2MjBDQzE5NkMyNjY4QTI2QjZGQkIzNkIwOTlFQjU1Qj"
                "M4QTU4QzExQzEyMDRERTVDIiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMDR2VVJh"
                "L1dJVmxpRE1HV3dtYUtKcmI3czJzSm5yVmJPS1dNRWNFZ1RlWEhNaEFvR01OY3"
                "NnVVVOd0tvK2g3emhhWEtGK0hHdzZYaEVqb0RLcmFsa1luTWpLZGtjd1JRSWhB"
                "SjdjTjRKNk5kVmZwbnZFSS9aZXVXQ1R2bnBhSmlyS05GY0Mzek1Pamd3akFpQW"
                "JLSTBmYlhnUzFSTGxvTmh4ZEhoVnE5b3pFV1ZFOWNJd1hETjNBeHF5WTNBU1FD"
                "dDArdS9pTlNERDZiWHZVVHRkbXQ0TnJ0bGJ4NFZ6dW1UcGZqUllwNGxNb0kvaD"
                "QzcFVUanA3VkZvWGJuS1dqVmhxTmFHbTU3N0s2SjY5N1haN1RRRT0ifSx7InZh"
                "bGlkYXRpb25fcHVibGljX2tleSI6IkVEQkVFMzBGQUU5MkVFRTg4RTFDNDk4ME"
                "QwOUVDRkRFOTlBMTE2RDA3OEVDMjE4NTdEQjFCNDdCNDI2NDE4RTQyOCIsIm1h"
                "bmlmZXN0IjoiSkFBQUFBSnhJZTIrNHcrdWt1N29qaHhKZ05DZXo5NlpvUmJRZU"
                "93aGhYMnh0SHRDWkJqa0tITWhBOVNtSER4T2lDTVRabDVuUnhycDJ5aldaNWdq"
                "eDJEcm9VRnJVNG5yRXFVN2RrY3dSUUloQUxkRUZlalkrcFVuZ2xpN3NUdmliME"
                "JtREhQN042aWtWRUJJNkg3SXdVMXpBaUJkc3lvU3FQY0MyTk1xZ0FuSFhIR2Rr"
                "QUl3QlFEMUFVZzlYOFpKTHlmY3dIQVNRQ3QxYktWek9NeFJRbVIzd05LNGRLZG"
                "9mSUdyeEU5U2p1TFI2UGE4QjVuMDhTWUo4SzYyZ2UrOWE2QnRaYWxFbS9IT2Rj"
                "ejBOQUZPY3ljckYvQ3RTQTQ9In1dfQ";
            tmpXpop[jss::validation][jss::unl][jss::signature] =
                "9CCA07A3EDD1334D5ADCB3730D8F3F9BD1E0C338100384C7B15B6A910F96BE"
                "4F46E3052B37E9FE2E7DC9918BD85B9E871923AE1BDD7144EE2A92F625064C"
                "570C";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: depth > 32
        // temMALFORMED - Import: !proof->isObject() && !proof->isArray()
        // DA: Catchall Error
        // temMALFORMED - Import: return false

        // temMALFORMED - Import: xpop proof did not contain the specified txn
        // hash
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::proof][jss::children]["D"]
                   [jss::children]["7"][jss::hash] =
                       "12D47E7D543E15F1EDBA91CDF335722727851BDDA8C2FF8924772AD"
                       "C6B522A29";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // // temMALFORMED - Import: depth > 32
        // // temMALFORMED - Import: !proof.isObject() && !proof.isArray()
        // // DA: CatchAll Error

        // temMALFORMED - Import: computed txroot does not match xpop txroot,
        // invalid xpop.
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::proof][jss::children]["3"]
                   [jss::hash] =
                       "22D47E7D543E15F1EDBA91CDF335722727851BDDA8C2FF8924772AD"
                       "C6B522A29";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: error parsing coins | phash | acroot in the
        // ledger section of XPOP.

        // temMALFORMED - Import: unl blob contained invalid validator entry,
        // skipping
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3"
                "JzIjpbeyJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21h"
                "S0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aG"
                "FYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBu"
                "dkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG"
                "9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJY"
                "dlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm"
                "5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsibWFuaWZlc3QiOiJKQUFB"
                "QUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQm"
                "prS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJF"
                "cVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQk"
                "k2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4"
                "WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUj"
                "ZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9D"
                "dFNBND0ifV19=";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob contained an invalid validator key,
        // skipping
        // {

        // }
        // temMALFORMED - Import: unl blob contained an invalid manifest,
        // skipping
        // {

        // }
        // // temMALFORMED - Import: unl blob list entry manifest master key did
        // // not match master key, skipping
        // // temMALFORMED - Import: unl blob list entry manifest signature
        // // invalid, skipping
        // // temMALFORMED - Import: validator nodepub did not appear in
        // validator
        // // list but did appear
        // // temMALFORMED - Import: validator nodepub key appears more than
        // once
        // // in data section

        // temMALFORMED - Import: validation inside xpop was not valid hex
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            Json::Value valData;
            valData["n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN"] =
                "not a hex";
            tmpXpop[jss::validation][jss::data] = valData;
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation message was not for computed ledger
        // hash
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            Json::Value valData;
            valData["n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN"] =
                "22800000012600000056292C0D012051A0829745427488A59B6525231634DC"
                "327F91589EB03F469ABF1C3CA32070625A501790AD68E9045001FDEED03F0A"
                "C8277F11F6E46E0ABD2DC38B1F8240D56B22E4F2732103D4A61C3C4E882313"
                "665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76463044022072"
                "427336342AE80B7AAE407CEA611B0DA680426B3C5894E52EE2D23641D09A73"
                "02203B131B68D3C5B6C5482312CC7D90D2CE131A9C46458967F2F98688B726"
                "C16719";
            tmpXpop[jss::validation][jss::data] = valData;
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // temMALFORMED - Import: validation inside xpop was not signed with a
        // signing key we recognise
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            Json::Value valData;
            valData["n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN"] =
                "22800000012600000056292C0D012051B0829745427488A59B6525231634DC"
                "327F91589EB03F469ABF1C3CA32070625A501790BD68E9045001FDEED03F0A"
                "C8277F11F6E46E0ABD2DC38B1F8240D56B22E4F2732103E4A61C3C4E882313"
                "665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76463054022072"
                "427336342AE80B7AAE407CEA611B0DA680426B3C5894E52EE2D23641D09A73"
                "02203B131B68D3C5B6C5482312CC7D90D2CE131A9C46458967F2F98688B726"
                "C16719";
            tmpXpop[jss::validation][jss::data] = valData;
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // temMALFORMED - Import: validation inside xpop was not correctly
        // signed
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            Json::Value valData;
            valData["n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN"] =
                "22800000012600000056292C0D012051B0829745427488A59B6525231634DC"
                "327F91589EB03F469ABF1C3CA32070625A501790BD68E9045001FDEED03F0A"
                "C8277F11F6E46E0ABD2DC38B1F8240D56B22E4F2732103D4A61C3C4E882313"
                "665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76463044022072"
                "427336342AE80B7AAE407CEA611B0DA680426B3C5894E52EE2D23641D09A73"
                "02203B131B68D3C5B6C5482312CC7D90D2CE131A9C46458967F2F98688B726"
                "C16719";
            tmpXpop[jss::validation][jss::data] = valData;
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation inside xpop was not able to be
        // parsed DA: Unknown Catch All

        // temMALFORMED - Import: xpop did not contain an 80% quorum for the txn
        // it purports to prove.
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            Json::Value valData;
            valData["n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN"] =
                "";
            valData["n9KXYzdZD8YpsNiChtMjP6yhvQAhkkh5XeSTbvYyV1waF8wkNnBT"] =
                "";
            tmpXpop[jss::validation][jss::data] = valData;
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: xpop inner txn did not contain a sequence
        // number or fee No Sequence
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "1200632200000000201B0000006C201D0000535968400000003B9ACA007321"
                "EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A9"
                "6C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF"
                "0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C"
                "5177C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // No Fee
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "12006322000000002400000002201B0000006C201D000053597321EDA8D46E"
                "11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440"
                "549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4"
                "375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519"
                "560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // Bad Fee - TODO
    }

    void
    testInvalidPreclaim(FeatureBitset features)
    {
        testcase("import invalid preclaim");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");

        env.fund(XRP(1000), alice, bob);
        env.close();

        //----------------------------------------------------------------------
        // preclaim

        // temDISABLED - disabled - CHECKED IN ENABLED
        // tefINTERNAL/temMALFORMED - during preclaim could not parse xpop,
        // bailing.
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation] = {};  // one of many ways to throw error
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // tefINTERNAL/temMALFORMED - during preclaim could not find
        // importSequence, bailing.
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "1200632200000000201B0000006C201D0000535968400000003B9ACA007321"
                "EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A9"
                "6C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF"
                "0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C"
                "5177C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // tefPAST_IMPORT_SEQ -
        {
            env(import(alice, loadXpop(ImportTCAccountSet::w_seed)),
                ter(tesSUCCESS));
            env(import(alice, loadXpop(ImportTCAccountSet::min)),
                ter(tefPAST_IMPORT_SEQ));
        }

        // tefINTERNAL/temMALFORMED - !vlInfo
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] = "YmFkSnNvbg==";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // tefPAST_IMPORT_VL_SEQ - sfImportSequence > vlInfo->first
        // {
        //     Json::Value tx = import(alice, loadXpop(ImportTCAccountSet::w_seed)); env(tx, ter(tefPAST_IMPORT_SEQ));
        // }

        // telIMPORT_VL_KEY_NOT_RECOGNISED - Import: (fromchain) key does not
        // match (tochain) key
        // {
        //     Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
        //     tmpXpop[jss::validation][jss::unl][jss::public_key] =
        //         "ED84D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
        //         "CDC1";
        //     Json::Value tx = import(alice, tmpXpop);
        //     env(tx, ter(telIMPORT_VL_KEY_NOT_RECOGNISED));
        // }
    }

    void
    testInvalidDoApply(FeatureBitset features)
    {
        testcase("import invalid doApply");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");

        env.fund(XRP(1000), alice, bob);
        env.close();

        //----------------------------------------------------------------------
        // doApply

        // temDISABLED - disabled
        // DA: Sanity Check

        // tefINTERNAL/temMALFORMED - ctx_.tx.isFieldPresent(sfBlob)
        {
            Json::Value tx;
            tx[jss::TransactionType] = jss::Import;
            tx[jss::Account] = alice.human();
            env(tx, ter(temMALFORMED));
        }

        // tefINTERNAL/temMALFORMED - !xpop
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation] = {};  // one of many ways to throw error
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // tefINTERNAL/temMALFORMED - during apply could not find importSequence
        // or fee, bailing.
        // No Fee
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "1200632200000000201B0000006C201D0000535968400000003B9ACA007321"
                "EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A9"
                "6C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF"
                "0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C"
                "5177C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // No Sequence

        // tefINTERNAL - initBal <= beast::zero
        // Sanity Check

        // tefINTERNAL - ImportSequence passed
        // Sanity Check (tefPAST_IMPORT_SEQ)

        // tefINTERNAL/temMALFORMED - !infoVL
        {
            Json::Value tmpXpop = loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] = "YmFkSnNvbg==";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // tefINTERNAL - current > infoVL->first
        // Sanity Check (tefPAST_IMPORT_VL_SEQ)
    }

    void
    testAccountSet(FeatureBitset features)
    {
        testcase("account set tx");

        using namespace test::jtx;
        using namespace std::literals;

        // w/ seed -> dne (bad signer)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(alice);
            env.memoize(bob);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import(bob, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, bob, ter(temMALFORMED));
            env.close();

            // confirm fee was not minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice);
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins);

            // confirm account does not exist
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle == nullptr);
        }

        // w/ seed -> dne
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(alice);
            env.memoize(bob);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            Json::Value tx = import(alice, loadXpop(ImportTCAccountSet::w_seed));
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + XRP(1000) + XRP(2));
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == 99'999'999'999'900'000);

            // confirm account exists
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle != nullptr);
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ regular key other -> dne (bad signer)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.memoize(alice);
            env.memoize(bob);
            env.memoize(carol);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx - wrong regular key
            Json::Value txBad =
                import(alice, loadXpop(ImportTCAccountSet::w_regular_key));
            txBad[jss::Sequence] = 0;
            txBad[jss::Fee] = 0;
            env(txBad, alice, sig(carol), ter(temMALFORMED));
            env.close();

            // confirm fee was not minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice);
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins);

            // confirm account does not exist
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle == nullptr);
        }

        // w/ regular key -> dne
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(alice);
            env.memoize(bob);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            Json::Value tx =
                import(alice, loadXpop(ImportTCAccountSet::w_regular_key));
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, sig(bob), ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + XRP(1000) + XRP(2));
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == 99'999'999'999'900'000);

            // confirm account exists
            auto const [acct, acctSle1] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle1 != nullptr);
            // alice cannnot sign
            env(noop(alice),
                sig(alice),
                fee(feeDrops),
                ter(tefMASTER_DISABLED));
            // bob cannot sign
            env(noop(alice), sig(bob), fee(feeDrops), ter(tefBAD_AUTH));
        }

        // // w/ signers list -> dne
        // {
        //     test::jtx::Env env{*this, makeNetworkConfig(21337)};

        //     auto const feeDrops = env.current()->fees().base;

        //     auto const master = Account("masterpassphrase");
        //     env(noop(master), fee(100'000), ter(tesSUCCESS));
        //     env.close();

        //     // init env
        //     auto const alice = Account("alice");
        //     auto const bob = Account("bob");
        //     auto const carol = Account("carol");
        //     env.memoize(alice);
        //     env.memoize(bob);
        //     env.memoize(carol);

        //     // confirm env
        //     auto const preCoins = env.current()->info().drops;
        //     BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
        //     auto const preAlice = env.balance(alice);
        //     BEAST_EXPECT(preAlice == XRP(0));

        //     // import tx
        //     auto const xpopJson = loadXpop(ImportTCAccountSet::w_signers);
        //     Json::Value tx = import(alice, xpopJson);
        //     tx[jss::Sequence] = 0;
        //     tx[jss::Fee] = 0;
        //     env(
        //         tx,
        //         alice,
        //         msig(bob, carol),
        //         fee(3 * feeDrops),
        //         ter(tesSUCCESS)
        //     );
        //     env.close();

        //     // confirm fee was minted
        //     auto const postAlice = env.balance(alice);
        //     BEAST_EXPECT(postAlice == preAlice + XRP(0.00001) + XRP(2));
        //     auto const postCoins = env.current()->info().drops;
        //     BEAST_EXPECT(postCoins == 99'999'999'999'900'000);

        //     // confirm signers not set
        //     auto const k = keylet::signers(bob);
        //     BEAST_EXPECT(env.current()->read(k) == nullptr);
        //     // alice cannnot sign
        //     env(noop(alice), sig(alice), fee(feeDrops),
        //     ter(tefMASTER_DISABLED));
        // }

        // w/ seed -> funded
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'899'980);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            env(import(alice, loadXpop(ImportTCAccountSet::w_seed)),
                ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + XRP(1000) - XRP(0.00001));
            std::cout << "postAlice: " << postAlice << "\n";
            std::cout << "postAlice=: " << (preAlice + XRP(1000) - XRP(0.00001))
                      << "\n";
            env.close();
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == 99'999'999'999'899'970);

            // confirm account exists
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle != nullptr);
            auto const feeDrops = env.current()->fees().base;
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ regular key -> funded
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'899'960);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            auto const xpopJson = loadXpop(ImportTCAccountSet::w_regular_key);
            Json::Value tx = import(alice, xpopJson);
            env(tx, alice, sig(bob), ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + XRP(1000) - drops(1));
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == 99'999'999'999'899'950);
            std::cout << "postAlice: " << postAlice << "\n";
            std::cout << "postAlice=: " << (preAlice + XRP(1000) - drops(1))
                      << "\n";

            // confirm account exists bob can sign
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle != nullptr);
            BEAST_EXPECT(!acctSle->isFieldPresent(sfRegularKey));
            env(noop(alice), sig(bob), fee(feeDrops), ter(tefBAD_AUTH));
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ signers -> funded
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'899'940);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            auto const xpopJson = loadXpop(ImportTCAccountSet::w_signers);
            Json::Value tx = import(alice, xpopJson);
            env(tx,
                alice,
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + XRP(1000) - drops(1));
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == 99'999'999'999'899'910);

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);
        }
    }

    void
    testAccountSetFlags(FeatureBitset features)
    {
        testcase("account set flags");

        using namespace test::jtx;
        using namespace std::literals;

        // account set flags not migrated
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'899'980);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            env(import(alice, loadXpop(ImportTCAccountSet::w_flags)),
                ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + drops(11));
            env.close();
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == 99'999'999'999'899'970);

            // confirm account exists
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle != nullptr);
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }
    }

    void
    testSetRegularKey(FeatureBitset features)
    {
        testcase("set regular key tx");

        using namespace test::jtx;
        using namespace std::literals;

        // w/ seed -> dne
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(alice);
            env.memoize(bob);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = loadXpop(ImportTCSetRegularKey::w_seed);
            Json::Value tx = import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + drops(12) + XRP(2));
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == 99'999'999'999'900'000);

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == bob.id());
            env(noop(alice), sig(bob), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ regular key -> dne
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.memoize(alice);
            env.memoize(bob);
            env.memoize(carol);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = loadXpop(ImportTCSetRegularKey::w_regular_key);
            Json::Value tx = import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, sig(bob), ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + drops(12) + XRP(2));

            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + feeDrops - feeDrops);

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == carol.id());
            env(noop(alice), sig(carol), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ signers -> dne
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const dave = Account("dave");
            env.memoize(alice);
            env.memoize(bob);
            env.memoize(carol);
            env.memoize(dave);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = loadXpop(ImportTCSetRegularKey::w_signers);
            Json::Value tx = import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(
                tx,
                alice,
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS)
            );
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + XRP(2) + feeDrops +
            XRP(0.000002)); std::cout << "POST ALICE: " << postAlice << "\n";
            std::cout << "POST ALICE CALC: " << (preAlice + XRP(2) + feeDrops
            + XRP(0.000002)) << "\n";

            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + feeDrops - feeDrops);

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);

            // confirm regular key
             auto const [acct, acctSle] =
                 accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle && acctSle->isFieldPresent(sfRegularKey) && 
                    acctSle->getAccountID(sfRegularKey) == dave.id());
            env(noop(alice), sig(dave), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ seed -> funded
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'899'960);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            auto const xpopJson = loadXpop(ImportTCSetRegularKey::w_seed);
            env(import(alice, xpopJson), ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice - feeDrops + drops(12));
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == 99'999'999'999'899'950);

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == bob.id());
            env(noop(alice), sig(bob), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ seed -> funded (update regular key)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'899'940);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, carol));
            env(noop(alice), sig(carol), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // import tx
            auto const xpopJson = loadXpop(ImportTCSetRegularKey::w_seed);
            env(import(alice, xpopJson), sig(alice), ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice - (3 * feeDrops) + drops(12));
            auto const postCoins = env.current()->info().drops;
            // BEAST_EXPECT(postCoins == preCoins - (3 * feeDrops));

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == bob.id());
            env(noop(alice), sig(bob), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ regular key -> funded (update regular key)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const dave = Account("dave");
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'899'920);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, dave));
            env(noop(alice), sig(dave), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // import tx
            auto const xpopJson = loadXpop(ImportTCSetRegularKey::w_regular_key);
            env(import(alice, xpopJson), sig(bob), ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice - (3 * feeDrops) + drops(12));
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins - (3 * feeDrops));

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == carol.id());
            env(noop(alice), sig(carol), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ signers list -> funded (update regular key)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const dave = Account("dave");
            auto const elsa = Account("elsa");
            env.fund(XRP(1000), alice, bob, carol, dave, elsa);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'899'900);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, elsa));
            env(noop(alice), sig(elsa), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // import tx
            auto const xpopJson = loadXpop(ImportTCSetRegularKey::w_signers);
            env(import(alice, xpopJson),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + drops(12) - feeDrops);
            std::cout << "POST ALICE: " << postAlice << "\n";
            std::cout << "POST ALICE CALC: " << (preAlice + drops(12)) <<
            "\n"; std::cout << "POST ALICE CALC: " << (preAlice + drops(12) -
            (2 * feeDrops)) << "\n";
            auto const postCoins = env.current()->info().drops;

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == dave.id());
            env(noop(alice), sig(carol), fee(feeDrops), ter(tesSUCCESS));
        }

        // seed -> funded (empty regular key)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'899'940);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, carol));
            env(noop(alice), sig(carol), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // import tx
            auto const xpopJson = loadXpop(ImportTCSetRegularKey::w_seed_empty);
            env(import(alice, xpopJson), sig(alice), ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice - (3 * feeDrops) + drops(12));
            auto const postCoins = env.current()->info().drops;
            // BEAST_EXPECT(postCoins == preCoins - (3 * feeDrops));

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(!acctSle->isFieldPresent(sfRegularKey));
        }

        // w/ regular key -> funded (empty regular key)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'899'940);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, carol));
            env(noop(alice), sig(carol), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // import tx
            auto const xpopJson =
                loadXpop(ImportTCSetRegularKey::w_regular_key_empty);
            env(import(alice, xpopJson), sig(bob), ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + drops(12) - (3 * feeDrops));
            auto const postCoins = env.current()->info().drops;
            // BEAST_EXPECT(postCoins == preCoins - (3 * feeDrops));

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(!acctSle->isFieldPresent(sfRegularKey));
            env(noop(alice), sig(alice), fee(feeDrops), ter(tesSUCCESS));
            env(noop(alice), sig(bob), fee(feeDrops), ter(tefBAD_AUTH));
            env(noop(alice), sig(carol), fee(feeDrops), ter(tefBAD_AUTH));
        }

        // w/ signers -> funded (empty regular key)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const dave = Account("dave");
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'899'920);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, dave));
            env(noop(alice), sig(dave), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // import tx
            auto const xpopJson =
                loadXpop(ImportTCSetRegularKey::w_signers_empty);
            env(import(alice, xpopJson),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + drops(12) - (2 * feeDrops));
            std::cout << "postAlice: " << postAlice << "\n";
            std::cout << "postAlice=: " << (preAlice - (2 * feeDrops)) << "\n";
            auto const postCoins = env.current()->info().drops;
            // std::cout << "postCoins: " << postCoins << "\n";
            // std::cout << "postCoins=: " << (preCoins - (5 * feeDrops)) << "\n";
            // BEAST_EXPECT(postCoins == preCoins - (5 * feeDrops));

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(!acctSle->isFieldPresent(sfRegularKey));
            env(noop(alice), sig(alice), fee(feeDrops), ter(tesSUCCESS));
            env(noop(alice), sig(bob), fee(feeDrops), ter(tefBAD_AUTH));
            env(noop(alice), sig(carol), fee(feeDrops), ter(tefBAD_AUTH));
            env(noop(alice), sig(dave), fee(feeDrops), ter(tefBAD_AUTH));
        }
    }

    void
    testSetRegularKeyFlags(FeatureBitset features)
    {
        testcase("set regular key flags");

        using namespace test::jtx;
        using namespace std::literals;

        // dne -> dont set flag
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(alice);
            env.memoize(bob);

            // import tx
            auto const xpopJson = loadXpop(ImportTCSetRegularKey::w_seed);
            Json::Value tx = import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // confirm lsfPasswordSpent is set
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(
                (acctSle->getFieldU32(sfFlags) & lsfPasswordSpent) == 0);
        }

        // funded -> set flag
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // import tx
            auto const xpopJson = loadXpop(ImportTCSetRegularKey::w_seed_zero);
            env(import(alice, xpopJson), ter(tesSUCCESS));
            env.close();

            // confirm lsfPasswordSpent is not set
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(
                (acctSle->getFieldU32(sfFlags) & lsfPasswordSpent) ==
                lsfPasswordSpent);
        }
    }

    void
    testSignersListSet(FeatureBitset features)
    {
        testcase("signers list set tx");

        using namespace test::jtx;
        using namespace std::literals;

        // w/ seed -> dne w/ seed (Bad Fee)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.memoize(alice);
            env.memoize(bob);
            env.memoize(carol);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson =
                loadXpop(ImportTCSignersListSet::w_seed_bad_fee);
            Json::Value tx = import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(
                postAlice == preAlice + drops(12) + XRP(2));
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == 99'999'999'999'900'000);
            std::cout << "POST COINS: " << postCoins << "\n";

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);
        }

        // w/ seed -> dne w/ seed
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.memoize(alice);
            env.memoize(bob);
            env.memoize(carol);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const burnAmt = XRP(2);
            auto const xpopJson = loadXpop(ImportTCSignersListSet::w_seed);
            Json::Value tx = import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(
                postAlice ==
                preAlice + drops(12) + burnAmt + XRP(2));
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == 99'999'999'999'900'000);

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);
        }

        // w/ regular key -> dne
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const dave = Account("dave");
            env.memoize(alice);
            env.memoize(bob);
            env.memoize(carol);
            env.memoize(dave);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const burnAmt = XRP(2);
            auto const xpopJson = loadXpop(ImportTCSignersListSet::w_regular_key);
            Json::Value tx = import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, sig(bob), ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(
                postAlice ==
                preAlice + feeDrops + XRP(2) + XRP(0.000002) + burnAmt);
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == 99'999'999'999'900'000);

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);
        }

        // // w/ signers -> dne
        // {
        //     test::jtx::Env env{*this, makeNetworkConfig(21337)};

        //     auto const feeDrops = env.current()->fees().base;

        //     // burn 100,000 xrp
        //     auto const master = Account("masterpassphrase");
        //     env(noop(master), fee(100'000), ter(tesSUCCESS));
        //     env.close();

        //     // init env
        //     auto const alice = Account("alice");
        //     auto const bob = Account("bob");
        //     auto const carol = Account("carol");
        //     env.memoize(alice);
        //     env.memoize(bob);
        //     env.memoize(carol);

        //     // confirm env
        //     auto const preCoins = env.current()->info().drops;
        //     BEAST_EXPECT(preCoins == 99'999'999'999'900'000);
        //     auto const preAlice = env.balance(alice);
        //     BEAST_EXPECT(preAlice == XRP(0));

        //     // import tx
        //     auto const burnAmt = XRP(2);
        //     auto const xpopJson = loadXpop(ImportTCSignersListSet::w_signers);
        //     Json::Value tx = import(alice, xpopJson);
        //     tx[jss::Sequence] = 0;
        //     tx[jss::Fee] = 0;
        //     env(tx,
        //         alice,
        //         msig(bob, carol),
        //         fee(3 * feeDrops),
        //         ter(tesSUCCESS));
        //     env.close();

        //     // confirm fee was minted
        //     auto const postAlice = env.balance(alice);
        //     BEAST_EXPECT(
        //         postAlice ==
        //         preAlice + feeDrops + XRP(2) + XRP(0.000002) + burnAmt);
        //     auto const postCoins = env.current()->info().drops;
        //     BEAST_EXPECT(postCoins == 99'999'999'999'900'000);

        //     // confirm signers not set
        //     auto const k = keylet::signers(alice);
        //     BEAST_EXPECT(env.current()->read(k) == nullptr);
        // }

        // w/ seed -> funded
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;
            // auto const totalCoins = drops(100'000'000'000'000'000);

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            // BEAST_EXPECT(preCoins == totalCoins - drops(100'000) - (3 * (2 *
            // feeDrops)));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            // auto const burnAmt = XRP(2);
            auto const xpopJson = loadXpop(ImportTCSignersListSet::w_seed);
            env(import(alice, xpopJson), ter(tesSUCCESS));
            env.close();

            // confirm fee minted
            auto const postAlice = env.balance(alice);
            // BEAST_EXPECT(postAlice == preAlice + (2 * feeDrops));
            std::cout << "POST ALICE: " << postAlice << "\n";
            std::cout << "POST ALICE CALC: " << (preAlice + (2 * feeDrops))
                      << "\n";
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins - feeDrops);

            // confirm signers set
            auto const [signers, signersSle] =
                signersKeyAndSle(*env.current(), alice);
            auto const signerEntries =
                signersSle->getFieldArray(sfSignerEntries);
            BEAST_EXPECT(signerEntries.size() == 2);
            BEAST_EXPECT(signerEntries[0u].getFieldU16(sfSignerWeight) == 1);
            BEAST_EXPECT(
                signerEntries[0u].getAccountID(sfAccount) == carol.id());
            BEAST_EXPECT(signerEntries[1u].getFieldU16(sfSignerWeight) == 1);
            BEAST_EXPECT(signerEntries[1u].getAccountID(sfAccount) == bob.id());

            // confirm multisign tx
            env.close();
            auto const aliceSeq = env.seq(alice);
            env(noop(alice),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();
            // BEAST_EXPECT(env.seq(alice) == aliceSeq + 1);
        }

        // w/ seed (empty) -> funded (has entries)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;
            // auto const totalCoins = drops(100'000'000'000'000'000);

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            // BEAST_EXPECT(preCoins == totalCoins - drops(100'000) - (3 * (2 *
            // feeDrops)));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // set the signers list
            env(signers(alice, 2, {{bob, 1}, {carol, 1}}));
            env(noop(alice),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();

            // import tx
            auto const burnAmt = XRP(2);
            auto const xpopJson = loadXpop(ImportTCSignersListSet::w_seed_empty);
            env(import(alice, xpopJson), ter(tesSUCCESS));
            env.close();

            // confirm fee minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(
                postAlice ==
                preAlice + burnAmt - (4 * feeDrops) + XRP(0.000002));
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins - (5 * feeDrops));

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);
        }

        // w/ regular key (empty) -> funded (has entries)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;
            // auto const totalCoins = drops(100'000'000'000'000'000);

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            // BEAST_EXPECT(preCoins == totalCoins...);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, bob));
            env(noop(alice), sig(bob), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // set the signers list
            env(signers(alice, 2, {{bob, 1}, {carol, 1}}));
            env(noop(alice),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();

            // import tx
            auto const burnAmt = XRP(2);
            auto const xpopJson =
                loadXpop(ImportTCSignersListSet::w_regular_key_empty);
            env(import(alice, xpopJson), sig(bob), ter(tesSUCCESS));
            env.close();

            // confirm fee minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(
                postAlice ==
                preAlice + burnAmt - (6 * feeDrops) + XRP(0.000002));
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins - (7 * feeDrops));

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == bob.id());
            env(noop(alice), sig(bob), fee(feeDrops), ter(tesSUCCESS));

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);
        }

        // w/ signers (empty) -> funded (has entries)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;
            // auto const totalCoins = drops(100'000'000'000'000'000);

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            // BEAST_EXPECT(preCoins == totalCoins...);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // set the signers list
            env(signers(alice, 2, {{bob, 1}, {carol, 1}}));
            env(noop(alice),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();

            // import tx
            auto const burnAmt = XRP(2);
            auto const xpopJson =
                loadXpop(ImportTCSignersListSet::w_signers_empty);
            env(import(alice, xpopJson),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();

            // confirm fee minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(
                postAlice ==
                preAlice + burnAmt - (2 * feeDrops) - XRP(0.000002));
            auto const postCoins = env.current()->info().drops;
            // BEAST_EXPECT(postCoins == preCoins - (7 * feeDrops));
            // std::cout << "postCoins: " << postCoins << "\n";
            // std::cout << "postCoins=: " << (preCoins - (7 * feeDrops)) <<
            // "\n";

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);
        }
    }

    void
    testImportSequence(FeatureBitset features)
    {
        testcase("import sequence");

        using namespace test::jtx;
        using namespace std::literals;

        // test tefPAST_IMPORT_SEQ
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            auto preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));
            env(import(alice, loadXpop(ImportTCAccountSet::w_seed)),
                ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + XRP(1000) - feeDrops);

            env(import(alice, loadXpop(ImportTCAccountSet::w_seed)),
                ter(tefPAST_IMPORT_SEQ));
            env.close();
            auto const failedAlice = env.balance(alice);
            BEAST_EXPECT(failedAlice == postAlice);
        }
    }

    void
    testMaxSupply(FeatureBitset features)
    {
        testcase("max supply");

        using namespace test::jtx;
        using namespace std::literals;

        // burn 100'000 coins
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == 100'000'000'000'000'000);
            // 100'000'000'000'000'000 - drops
            // 100'000'000'000 - xrp

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000'000'000), ter(tesSUCCESS));
            env.close();

            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == envCoins - drops(100'000'000'000));
 
            auto const alice = Account("alice");
            env.memoize(alice);

            auto preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));
            
            STAmount burnFee = XRP(1000) + XRP(2);
            auto const xpopJson = loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + burnFee);
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + burnFee);
        }

        // burn all coins
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == 100'000'000'000'000'000);

            // burn all but 1,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(envCoins - drops(1'000'000'000)), ter(tesSUCCESS));
            env.close();

            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == XRP(1000));

            auto const alice = Account("alice");
            env.memoize(alice);

            auto preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));
            
            STAmount burnFee = XRP(1000) + XRP(2);
            auto const xpopJson = loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();
            
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + burnFee);
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + burnFee);
        }

        // burn no coins
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == 100'000'000'000'000'000);

            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == XRP(100'000'000'000));

            auto const alice = Account("alice");
            env.memoize(alice);

            auto preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));
            
            STAmount burnFee = XRP(1000) + XRP(2);
            auto const xpopJson = loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tefINTERNAL));
            env.close();
            
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice);
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins);
        }
    }

    void
    testMinMax(FeatureBitset features)
    {
        testcase("min max");

        using namespace test::jtx;
        using namespace std::literals;

        // w/ seed -> dne (min)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;
            auto const envCoins = env.current()->info().drops;
            auto const totalCoins = drops(100'000'000'000'000'000);

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000), ter(tesSUCCESS));
            env.close();
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == totalCoins - drops(100'000));

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = loadXpop(ImportTCAccountSet::min);
            Json::Value tx = import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + drops(1) + XRP(2));
            auto const postCoins = env.current()->info().drops;
            std::cout << "POST COINS: " << postCoins << "\n";
            BEAST_EXPECT(postCoins == 99'999'999'999'900'000);

            // confirm account exists
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle != nullptr);
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // // w/ seed -> dne (max)
        // {
        //     test::jtx::Env env{*this, makeNetworkConfig(21337)};

        //     auto const feeDrops = env.current()->fees().base;
        //     auto const envCoins = env.current()->info().drops;
        //     auto const totalCoins = drops(100'000'000'000'000'000);

        //     // init env
        //     auto const alice = Account("alice");
        //     env.memoize(alice);

        //     // confirm env
        //     auto const preCoins = env.current()->info().drops;
        //     BEAST_EXPECT(preCoins == totalCoins);
        //     auto const preAlice = env.balance(alice);
        //     BEAST_EXPECT(preAlice == XRP(0));

        //     // import tx
        //     auto const xpopJson = loadXpop(ImportTCAccountSet::max);
        //     Json::Value tx = import(alice, xpopJson);
        //     tx[jss::Sequence] = 0;
        //     env(tx, alice, ter(tesSUCCESS));
        //     env.close();

        //     // confirm fee was minted
        //     auto const postAlice = env.balance(alice);
        //     BEAST_EXPECT(
        //         postAlice == preAlice + XRP(999'999'999'999'000'000) + XRP(2));
        //     auto const postCoins = env.current()->info().drops;
        //     BEAST_EXPECT(postCoins == 999'999'999'999'900'000);
        //     // std::cout << "POST COINS: " << postCoins << "\n";

        //     // confirm account exists
        //     auto const [acct, acctSle] =
        //         accountKeyAndSle(*env.current(), alice);
        //     BEAST_EXPECT(acctSle != nullptr);
        //     env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        // }
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testWithFeats(all);
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testComputeStartingBalance(features);
        testIsHex(features);
        testIsBase58(features);
        testIsBase64(features);
        testParseUint64(features);
        testSyntaxCheckProofObject(features);
        testSyntaxCheckXPOP(features);
        testGetVLInfo(features);
        testEnabled(features);
        testInvalidPreflight(features);
        testInvalidPreclaim(features);
        testInvalidDoApply(features);
        testAccountSet(features);
        testAccountSetFlags(features);
        testSetRegularKey(features);
        testSetRegularKeyFlags(features);
        testSignersListSet(features);
        testImportSequence(features);
        testMaxSupply(features);
        testMinMax(features);
    }
};

BEAST_DEFINE_TESTSUITE(Import, app, ripple);

}  // namespace test
}  // namespace ripple
