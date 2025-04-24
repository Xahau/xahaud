//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL Labs

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
#include <ripple/app/misc/AmendmentTable.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/tx/impl/Import.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/ledger/Directory.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Import.h>
#include <ripple/protocol/jss.h>
#include <test/app/Import_json.h>
#include <test/jtx.h>

#define BEAST_REQUIRE(x)     \
    {                        \
        BEAST_EXPECT(!!(x)); \
        if (!(x))            \
            return;          \
    }
#define M(m) memo(m, "", "")

namespace ripple {
namespace test {

class Import_test : public beast::unit_test::suite
{
    std::vector<std::string> const keys = {
        "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1"};

    static std::pair<uint256, std::shared_ptr<SLE const>>
    accountKeyAndSle(ReadView const& view, jtx::Account const& account)
    {
        auto const k = keylet::account(account);
        return {k.key, view.read(k)};
    }

    static std::pair<uint256, std::shared_ptr<SLE const>>
    feesKeyAndSle(ReadView const& view)
    {
        auto const k = keylet::fees();
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

    static std::uint32_t
    importVLSequence(jtx::Env const& env, PublicKey const& pk)
    {
        auto const sle = env.le(keylet::import_vlseq(pk));
        if (sle && sle->isFieldPresent(sfImportSequence))
            return (*sle)[sfImportSequence];
        return 0;
    }

    bool
    hasUNLReport(jtx::Env const& env)
    {
        auto const slep = env.le(keylet::UNLReport());
        return slep != nullptr;
    }

    void
    incLgrSeqForAccDel(
        jtx::Env& env,
        jtx::Account const& acc,
        std::uint32_t margin = 0)
    {
        int const delta = [&]() -> int {
            if (env.seq(acc) + 255 > env.current()->seq())
                return env.seq(acc) - env.current()->seq() + 255 - margin;
            return 0;
        }();
        BEAST_EXPECT(margin == 0 || delta >= 0);
        for (int i = 0; i < delta; ++i)
            env.close();
        BEAST_EXPECT(env.current()->seq() == env.seq(acc) + 255 - margin);
    }

    void
    testComputeStartingBalance(FeatureBitset features)
    {
        testcase("import header - computeStartingBonus");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, network::makeNetworkVLConfig(21337, keys)};

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
        // BEAST_EXPECT(result2 == 0xFFAABBCCDD22);

        std::optional<uint64_t> result3 = parse_uint64("2147483647");
        BEAST_EXPECT(result3 == 2147483647);

        std::optional<uint64_t> result4 = parse_uint64("18446744073709551615");
        BEAST_EXPECT(result4 == 18446744073709551615ull);
    }

    void
    testSyntaxCheckProofArray(FeatureBitset features)
    {
        testcase("import utils - syntaxCheckProof: is array");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, network::makeNetworkVLConfig(21337, keys)};

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        // XPOP.transaction.proof list xpop is disabled
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::proof] = Json::arrayValue;
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // // XPOP.transaction.proof list should be exactly 16 entries
        //  {
        //     Json::Value tmpXpop =
        //     import::loadXpop(ImportTCAccountSet::w_seed);
        //     tmpXpop[jss::transaction][jss::proof] = Json::arrayValue;
        //     for(int i = 0; i < 16; i++) {
        //         tmpXpop[jss::transaction][jss::proof].append("E000CC0736630D66DAB573A2642E1BD0646DFB13A871B2CCFA6E4226F477D88C");
        //     }
        //     Json::Value const tx = import::import(alice, tmpXpop);
        //     env(tx, ter(temMALFORMED));
        // }
        //  // XPOP.transaction.proof list entry missing or wrong format (should
        //  be hex string with 64 characters)
        //  {
        //     Json::Value tmpXpop =
        //     import::loadXpop(ImportTCAccountSet::w_seed);
        //     tmpXpop[jss::transaction][jss::proof] = Json::arrayValue;
        //     for(int i = 0; i < 16; i++) {
        //         tmpXpop[jss::transaction][jss::proof].append("wrong format");
        //     }
        //     Json::Value const tx = import::import(alice, tmpXpop);
        //     env(tx, ter(temMALFORMED));
        // }
        //  // XPOP.transaction.proof list entry has wrong format
        //  {
        //     Json::Value tmpXpop =
        //     import::loadXpop(ImportTCAccountSet::w_seed);
        //     tmpXpop[jss::transaction][jss::proof] = Json::arrayValue;
        //     Json::Value child1;
        //     child1["children"] = Json::arrayValue;
        //     child1["hash"] =
        //     "31C2D27644F0E6D81AF332F466F71D43C25946A45FE43F121A500E22003F39A4";
        //     child1["key"] =
        //     "60748F98318DB6A39737D0C1BE5614AEBD7F1ACEE5FB16E82D49F16BB13BA87F";
        //     tmpXpop[jss::transaction][jss::proof].append(child1);
        //     Json::Value const tx = import::import(alice, tmpXpop);
        //     env(tx, ter(temMALFORMED));
        //  }
    }

    void
    testSyntaxCheckProofObject(FeatureBitset features)
    {
        testcase("import utils - syntaxCheckProof: is object");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, network::makeNetworkVLConfig(21337, keys)};

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

        test::jtx::Env env{*this, network::makeNetworkVLConfig(21337, keys)};

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
        // XPOP.ledger.index missing or wrong format
        // invalid xpop.ledger.index (must be int)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": "not an int",
                    "index": "1"
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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
                    "index": 1,
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

        test::jtx::Env env{*this, network::makeNetworkVLConfig(21337, keys)};

        std::string strJson = R"json({
            "ledger": {
                "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                "close": 738786851,
                "coins": "99999998999999868",
                "cres": 10,
                "flags": 10,
                "index": 1,
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
            Env env{*this, network::makeNetworkVLConfig(21337, keys), amend};

            auto const feeDrops = env.current()->fees().base;

            // setup env
            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            // burn 1000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(1'000'000'000), ter(tesSUCCESS));
            env.close();

            auto const txResult =
                withImport ? ter(tesSUCCESS) : ter(temDISABLED);

            // IMPORT - Account Set
            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                fee(feeDrops * 10),
                txResult);
            env.close();
        }
    }

    void
    testInvalidPreflight(FeatureBitset features)
    {
        testcase("import invalid preflight");

        using namespace test::jtx;
        using namespace std::literals;

        //----------------------------------------------------------------------
        // preflight

        // temDISABLED - disabled
        // !ctx.rules.enabled(featureImport)
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(21337, keys),
                features - featureImport};

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                ter(temDISABLED));
        }

        // temDISABLED - disabled
        // !ctx.rules.enabled(featureHooksUpdate1) &&
        // ctx.tx.isFieldPresent(sfIssuer)
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(21337, keys),
                features - featureHooksUpdate1};

            auto const alice = Account("alice");
            auto const issuer = Account("issuer");
            env.fund(XRP(1000), alice, issuer);
            env.close();

            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                import::issuer(issuer),
                ter(temDISABLED));
        }

        // temMALFORMED - Import: xpop did not contain an 80% quorum for the txn
        // it purports to prove.
        for (bool const withXahauV1 : {true, false})
        {
            auto const amend = withXahauV1 ? features : features - fixXahauV1;
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys), amend};

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value valData = tmpXpop[jss::validation][jss::data];
            valData["n9MNStvsHZxrCYnShytCfKMCXBcq8wJjR6fLTS7L2Q8qoFVNioQ8"] =
                "";
            valData["n9MTTavZe6EPqqyQ27pJbNWpfHw8ZNspVgznrwx5HWm7cdTjKQie"] =
                "";
            if (withXahauV1)
            {
                valData
                    ["n94J5LCRu9bBWrJiwRWujvuVECVPWbXFcQ2VN38qLD378F5pDSDM"] =
                        "";
            }
            tmpXpop[jss::validation][jss::data] = valData;
            auto const txResult =
                withXahauV1 ? ter(temMALFORMED) : ter(temMALFORMED);
            env(import::import(alice, tmpXpop), txResult);
        }

        test::jtx::Env env{*this, network::makeNetworkVLConfig(21337, keys)};

        auto const feeDrops = env.current()->fees().base;

        // burn 1000 xrp
        auto const master = Account("masterpassphrase");
        env(noop(master), fee(1'000'000'000), ter(tesSUCCESS));
        env.close();

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const dave = Account("dave");

        env.fund(XRP(1000), alice, bob, carol, dave);
        env.close();

        // temMALFORMED
        // Issuer cannot be the source account.
        {
            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                import::issuer(alice),
                ter(temMALFORMED));
        }

        // telINSUF_FEE_P - sfFee cannot be 0
        {
            Json::Value tx = import::import(
                alice, import::loadXpop(ImportTCAccountSet::w_seed));
            STAmount const& fee = XRP(0);
            tx[jss::Fee] = fee.getJson(JsonOptions::none);
            env(tx, ter(telINSUF_FEE_P));
        }

        // temMALFORMED - blob was more than 512kib
        // DA: blob.resize(513 * 1024); fails with Boost
        // {
        //     ripple::Blob blob;
        //     blob.resize(513 * 1024);
        //     Json::Value tx = import::import(alice,
        //     import::loadXpop(ImportTCAccountSet::w_seed));
        //     tx[sfBlob.jsonName] = strHex(blob); env(tx, ter(temMALFORMED));
        // }

        // temMALFORMED - sfAmount field must be in drops
        {
            Json::Value tx = import::import(
                alice, import::loadXpop(ImportTCAccountSet::w_seed));
            STAmount const& amount = XRP(-1);
            tx[jss::Amount] = amount.getJson(JsonOptions::none);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - !xpop | XPOP.validation is not a JSON object
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation] = {};  // one of many ways to throw error
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation.unl.public_key was not valid hex
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::public_key] = "not a hex";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation.unl.public_key was not a recognised
        // public key type
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::public_key] =
                "0084D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
                "CDC1";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // getInnerTxn - !xpop
        // DA: Duplicate -

        // getInnerTxn - failed to deserialize tx blob inside xpop (invalid hex)
        // DA: Duplicate - "XPOP.transaction.blob missing or wrong format"

        // getInnerTxn - failed to deserialize tx meta inside xpop (invalid hex)
        // DA: Duplicate - "XPOP.transaction.meta missing or wrong format"

        // getInnerTxn - failed to deserialize tx blob/meta inside xpop
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] = "DEADBEEF";
            tmpXpop[jss::transaction][jss::meta] = "DEADBEEF";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - !stpTrans
        // DA: Duplicate - getInnerTxn (Any Failure)

        // temMALFORMED - Import: attempted to import xpop containing an emitted
        // or pseudo txn.
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
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
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: inner txn lacked transaction result
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::meta] =
                "201C00000006F8E5110061250000005655463E39A6AFDDA77DBF3591BF3C2A"
                "4BE9BB8D9113BF6D0797EB403C3D0D894FEF5692FA6A9FC8EA6018D5D16532"
                "D7795C91BFB0831355BDFDA177E86C8BF997985FE624000000026240000000"
                "773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481"
                "14AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F1";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: inner txn did not have a tesSUCCESS or tec
        // result
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::meta] =
                "201C00000006F8E5110061250000005655463E39A6AFDDA77DBF3591BF3C2A"
                "4BE9BB8D9113BF6D0797EB403C3D0D894FEF5692FA6A9FC8EA6018D5D16532"
                "D7795C91BFB0831355BDFDA177E86C8BF997985FE624000000026240000000"
                "773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481"
                "14AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F103103C";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: import and txn inside xpop must be signed by
        // the same account
        {
            Json::Value const tmpXpop =
                import::loadXpop(ImportTCAccountSet::w_seed);
            env(import::import(bob, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: attempted to import xpop containing a txn with
        // a sfNetworkID field.
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "120003210000535922000000002400000002201B00000069201D0000535968"
                "400000003B9ACA0073210388935426E0D08083314842EDFBB2D517BD47699F"
                "9A4527318A8E10468C97C0527446304402200E40CA821A7BFE347448FFA654"
                "0F67C7FFBF0287756D324DFBADCEDE2B23782C02207BE1B1294F1A1D5AC219"
                "90740B78F9C0693431237D6E07FE84228082986E50FF8114AE123A8556F3CF"
                "91154711376AFB0F894F832B3D";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: OperationLimit missing from inner xpop txn.
        // outer txid:
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "12000322000000002400000002201B0000006C68400000003B9ACA007321ED"
                "A8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C"
                "747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F"
                "232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C51"
                "77C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: Wrong network ID for OperationLimit in inner
        // txn. outer txid:
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "12000322000000002400000002201B0000006C201D0000535A68400000003B"
                "9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62"
                "ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593EC"
                "C21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF"
                "3EC206AD3C5177C519560F8114AE123A8556F3CF91154711376AFB0F894F83"
                "2B3D";
            env(import::import(alice, tmpXpop), ter(telWRONG_NETWORK));
        }

        // temMALFORMED - Import: inner txn must be an AccountSet, SetRegularKey
        // or SignerListSet transaction.
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "12006322000000002400000002201B0000006C201D0000535968400000003B"
                "9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62"
                "ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593EC"
                "C21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF"
                "3EC206AD3C5177C519560F8114AE123A8556F3CF91154711376AFB0F894F83"
                "2B3D";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: outer and inner txns were (multi) signed with
        // different keys.
        {
            auto const xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_signers);
            env(import::import(alice, xpopJson),
                msig(bob, dave),
                fee((3 * feeDrops) * 10),
                ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - Import: outer and inner txns were (multi) signed with
        // different keys. - empty innerSigners
        {
            Json::Value xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_signers);
            xpopJson[jss::transaction][jss::blob] =
                "12000C22000000002400000014201B0000002B201D00005359202300000002"
                "6840000000001E84B073008114AE123A8556F3CF91154711376AFB0F894F83"
                "2B3DF3F1F4EB1300018114AA266540F7DACC27E264B75ED0A5ED7330BFB614"
                "E1EB1300018114D91B8EE5C7ABF632469D4C0907C5E40C8B8F79B3E1F1";
            env(import::import(alice, xpopJson),
                msig(bob, carol),
                fee((3 * feeDrops) * 10),
                ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - Import: outer or inner txn was missing signers.
        // different keys.
        {
            Json::Value xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_signers);
            xpopJson[jss::transaction][jss::blob] =
                "12000C22000000002400000014201B0000002B201D00005359202300000002"
                "6840000000001E84B073008114AE123A8556F3CF91154711376AFB0F894F83"
                "2B3DF4EB1300018114AA266540F7DACC27E264B75ED0A5ED7330BFB614E1EB"
                "1300018114D91B8EE5C7ABF632469D4C0907C5E40C8B8F79B3E1F1";
            env(import::import(alice, xpopJson),
                fee((3 * feeDrops) * 10),
                ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - Import: outer and inner txns were signed with
        // different keys.
        {
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_regular_key);
            env(import::import(alice, xpopJson),
                fee(feeDrops * 10),
                sig(carol),
                ter(temMALFORMED));
        }

        // temMALFORMED - Import: inner txn signature verify failed
        {
            Json::Value xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_signers);
            xpopJson[jss::transaction][jss::blob] =
                "12000C2200000008240000001A201B000003B9201D00005359202300000000"
                "6840000000001E84B073008114AE123A8556F3CF91154711376AFB0F894F83"
                "2B3DF3E0107321028949021029D5CC87E78BCF053AFEC0CAFD15108EC119EA"
                "AFEC466F5C095407BF74473045022100BE132424E6E6304574F4BB5F6E287A"
                "63429482CE7E0437B443FC1457F4A3830002204B909A44FF9DFFD99AF751B0"
                "40830E53830FE3CA591449600372BED85FDBC33C8114B389FBCED0AF9DCDFF"
                "62900BFAEFA3EB872D8A96E1E010732102691AC5AE1C4C333AE5DF8A93BDC4"
                "95F0EEBFC6DB0DA7EB6EF808F3AFC006E3FE74473045022100D235706A0AB2"
                "407EC5D62F9130E9A98A70D90CE0D69D749C37272DFC975BD79002207D8AED"
                "0BF31A5E3290920B72EAFE22AB6B09814466372E09B067779C4E103FBE8114"
                "F51DFC2A09D62CBBA1DFBDD4691DAC96AD98B90FE1F1";
            env(import::import(alice, xpopJson),
                msig(bob, carol),
                fee((3 * feeDrops) * 10),
                ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - Import: failed to deserialize manifest on txid
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::manifest] = "YmFkSnNvbg==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: manifest master key did not match top level
        // master key in unl section of xpop
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::manifest] =
                "JAAAAAFxIe2E1ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+"
                "b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338"
                "jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/"
                "ikfgf9SZOlOGcBcBJAw44PLjH+"
                "HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+"
                "IILJIiIKU/+1Uxx0FRpQbMDA==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: manifest signature invalid
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::manifest] =
                "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+"
                "b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkA3UjfY5zOEkhq31tU4338"
                "jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/"
                "ikfgf9SZOlOGcBcBJAw34PLjH+"
                "HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+"
                "IILJIiIKU/+1Uxx0FRpQbMDA==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob not signed correctly
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::signature] =
                "949F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578"
                "943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1"
                "F70D";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob not signed correctly
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::signature] = "not a hex";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob was not valid json (after base64
        // decoding)
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] = "YmFkSnNvbg==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob json (after base64 decoding) lacked
        // required field (sequence) and/or types
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJleHBpcmF0aW9uIjo3NDEzOTg0MDAsInZhbGlkYXRvcnMiOlt7InZhbGlkYX"
                "Rpb25fcHVibGljX2tleSI6IkVEMzhCRDQ0NUFGRDYyMTU5NjIwQ0MxOTZDMjY2"
                "OEEyNkI2RkJCMzZCMDk5RUI1NUIzOEE1OEMxMUMxMjA0REU1QyIsIm1hbmlmZX"
                "N0IjoiSkFBQUFBSnhJZTA0dlVSYS9XSVZsaURNR1d3bWFLSnJiN3Myc0puclZi"
                "T0tXTUVjRWdUZVhITWhBb0dNTmNzZ1VVTndLbytoN3poYVhLRitIR3c2WGhFam"
                "9ES3JhbGtZbk1qS2RrY3dSUUloQUo3Y040SjZOZFZmcG52RUkvWmV1V0NUdm5w"
                "YUppcktORmNDM3pNT2pnd2pBaUFiS0kwZmJYZ1MxUkxsb05oeGRIaFZxOW96RV"
                "dWRTljSXdYRE4zQXhxeVkzQVNRQ3QwK3UvaU5TREQ2Ylh2VVR0ZG10NE5ydGxi"
                "eDRWenVtVHBmalJZcDRsTW9JL2g0M3BVVGpwN1ZGb1hibktXalZocU5hR201Nz"
                "dLNko2OTdYWjdUUUU9In0seyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFREJF"
                "RTMwRkFFOTJFRUU4OEUxQzQ5ODBEMDlFQ0ZERTk5QTExNkQwNzhFQzIxODU3RE"
                "IxQjQ3QjQyNjQxOEU0MjgiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEv"
                "V0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2"
                "dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFK"
                "N2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYk"
                "tJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0"
                "MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oND"
                "NwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9XX0=";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }
        // temMALFORMED - Import: unl blob json (after base64 decoding) wrong
        // required field (sequence) and/or types
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6IjIiLCJleHBpcmF0aW9uIjo3NDEzOTg0MDAsInZhbGlkYX"
                "RvcnMiOlt7InZhbGlkYXRpb25fcHVibGljX2tleSI6IkVEMzhCRDQ0NUFGRDYy"
                "MTU5NjIwQ0MxOTZDMjY2OEEyNkI2RkJCMzZCMDk5RUI1NUIzOEE1OEMxMUMxMj"
                "A0REU1QyIsIm1hbmlmZXN0IjoiSkFBQUFBSnhJZTA0dlVSYS9XSVZsaURNR1d3"
                "bWFLSnJiN3Myc0puclZiT0tXTUVjRWdUZVhITWhBb0dNTmNzZ1VVTndLbytoN3"
                "poYVhLRitIR3c2WGhFam9ES3JhbGtZbk1qS2RrY3dSUUloQUo3Y040SjZOZFZm"
                "cG52RUkvWmV1V0NUdm5wYUppcktORmNDM3pNT2pnd2pBaUFiS0kwZmJYZ1MxUk"
                "xsb05oeGRIaFZxOW96RVdWRTljSXdYRE4zQXhxeVkzQVNRQ3QwK3UvaU5TREQ2"
                "Ylh2VVR0ZG10NE5ydGxieDRWenVtVHBmalJZcDRsTW9JL2g0M3BVVGpwN1ZGb1"
                "hibktXalZocU5hR201NzdLNko2OTdYWjdUUUU9In0seyJ2YWxpZGF0aW9uX3B1"
                "YmxpY19rZXkiOiJFREJFRTMwRkFFOTJFRUU4OEUxQzQ5ODBEMDlFQ0ZERTk5QT"
                "ExNkQwNzhFQzIxODU3REIxQjQ3QjQyNjQxOEU0MjgiLCJtYW5pZmVzdCI6IkpB"
                "QUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0"
                "VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxr"
                "WW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTk"
                "ZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3"
                "WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bV"
                "RwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3"
                "WFo3VFFFPSJ9XX0=";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }
        // temMALFORMED - Import: unl blob json (after base64 decoding)
        // lacked required field (expiration) and/or types
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwidmFsaWRhdG9ycyI6W3sidmFsaWRhdGlvbl9wdWJsaW"
                "Nfa2V5IjoiRUQzOEJENDQ1QUZENjIxNTk2MjBDQzE5NkMyNjY4QTI2QjZGQkIz"
                "NkIwOTlFQjU1QjM4QTU4QzExQzEyMDRERTVDIiwibWFuaWZlc3QiOiJKQUFBQU"
                "FKeEllMDR2VVJhL1dJVmxpRE1HV3dtYUtKcmI3czJzSm5yVmJPS1dNRWNFZ1Rl"
                "WEhNaEFvR01OY3NnVVVOd0tvK2g3emhhWEtGK0hHdzZYaEVqb0RLcmFsa1luTW"
                "pLZGtjd1JRSWhBSjdjTjRKNk5kVmZwbnZFSS9aZXVXQ1R2bnBhSmlyS05GY0Mz"
                "ek1Pamd3akFpQWJLSTBmYlhnUzFSTGxvTmh4ZEhoVnE5b3pFV1ZFOWNJd1hETj"
                "NBeHF5WTNBU1FDdDArdS9pTlNERDZiWHZVVHRkbXQ0TnJ0bGJ4NFZ6dW1UcGZq"
                "UllwNGxNb0kvaDQzcFVUanA3VkZvWGJuS1dqVmhxTmFHbTU3N0s2SjY5N1haN1"
                "RRRT0ifSx7InZhbGlkYXRpb25fcHVibGljX2tleSI6IkVEQkVFMzBGQUU5MkVF"
                "RTg4RTFDNDk4MEQwOUVDRkRFOTlBMTE2RDA3OEVDMjE4NTdEQjFCNDdCNDI2ND"
                "E4RTQyOCIsIm1hbmlmZXN0IjoiSkFBQUFBSnhJZTA0dlVSYS9XSVZsaURNR1d3"
                "bWFLSnJiN3Myc0puclZiT0tXTUVjRWdUZVhITWhBb0dNTmNzZ1VVTndLbytoN3"
                "poYVhLRitIR3c2WGhFam9ES3JhbGtZbk1qS2RrY3dSUUloQUo3Y040SjZOZFZm"
                "cG52RUkvWmV1V0NUdm5wYUppcktORmNDM3pNT2pnd2pBaUFiS0kwZmJYZ1MxUk"
                "xsb05oeGRIaFZxOW96RVdWRTljSXdYRE4zQXhxeVkzQVNRQ3QwK3UvaU5TREQ2"
                "Ylh2VVR0ZG10NE5ydGxieDRWenVtVHBmalJZcDRsTW9JL2g0M3BVVGpwN1ZGb1"
                "hibktXalZocU5hR201NzdLNko2OTdYWjdUUUU9In1dfQ==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }
        // temMALFORMED - Import: unl blob json (after base64 decoding) wrong
        // required field (expiration) and/or types
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6Ijc0MTM5ODQwMCIsInZhbGlkYX"
                "RvcnMiOlt7InZhbGlkYXRpb25fcHVibGljX2tleSI6IkVEMzhCRDQ0NUFGRDYy"
                "MTU5NjIwQ0MxOTZDMjY2OEEyNkI2RkJCMzZCMDk5RUI1NUIzOEE1OEMxMUMxMj"
                "A0REU1QyIsIm1hbmlmZXN0IjoiSkFBQUFBSnhJZTA0dlVSYS9XSVZsaURNR1d3"
                "bWFLSnJiN3Myc0puclZiT0tXTUVjRWdUZVhITWhBb0dNTmNzZ1VVTndLbytoN3"
                "poYVhLRitIR3c2WGhFam9ES3JhbGtZbk1qS2RrY3dSUUloQUo3Y040SjZOZFZm"
                "cG52RUkvWmV1V0NUdm5wYUppcktORmNDM3pNT2pnd2pBaUFiS0kwZmJYZ1MxUk"
                "xsb05oeGRIaFZxOW96RVdWRTljSXdYRE4zQXhxeVkzQVNRQ3QwK3UvaU5TREQ2"
                "Ylh2VVR0ZG10NE5ydGxieDRWenVtVHBmalJZcDRsTW9JL2g0M3BVVGpwN1ZGb1"
                "hibktXalZocU5hR201NzdLNko2OTdYWjdUUUU9In0seyJ2YWxpZGF0aW9uX3B1"
                "YmxpY19rZXkiOiJFREJFRTMwRkFFOTJFRUU4OEUxQzQ5ODBEMDlFQ0ZERTk5QT"
                "ExNkQwNzhFQzIxODU3REIxQjQ3QjQyNjQxOEU0MjgiLCJtYW5pZmVzdCI6IkpB"
                "QUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0"
                "VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxr"
                "WW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTk"
                "ZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3"
                "WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bV"
                "RwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3"
                "WFo3VFFFPSJ9XX0=";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }
        // temMALFORMED - Import: unl blob json (after base64 decoding)
        // lacked required field (effective) and/or types
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJlZmZlY3Rpdm"
                "UiOiI3NDEzOTg0MDAiLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1Ymxp"
                "Y19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQj"
                "M2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFB"
                "QUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVG"
                "VYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5N"
                "aktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQz"
                "N6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WERO"
                "M0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZm"
                "pSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3"
                "VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRU"
                "VFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0"
                "MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMDR2VVJhL1dJVmxpRE1HV3"
                "dtYUtKcmI3czJzSm5yVmJPS1dNRWNFZ1RlWEhNaEFvR01OY3NnVVVOd0tvK2g3"
                "emhhWEtGK0hHdzZYaEVqb0RLcmFsa1luTWpLZGtjd1JRSWhBSjdjTjRKNk5kVm"
                "ZwbnZFSS9aZXVXQ1R2bnBhSmlyS05GY0Mzek1Pamd3akFpQWJLSTBmYlhnUzFS"
                "TGxvTmh4ZEhoVnE5b3pFV1ZFOWNJd1hETjNBeHF5WTNBU1FDdDArdS9pTlNERD"
                "ZiWHZVVHRkbXQ0TnJ0bGJ4NFZ6dW1UcGZqUllwNGxNb0kvaDQzcFVUanA3VkZv"
                "WGJuS1dqVmhxTmFHbTU3N0s2SjY5N1haN1RRRT0ifV19";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }
        // temMALFORMED - Import: unl blob json (after base64 decoding)
        // lacked required field (validators) and/or types
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwfQ==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }
        // temMALFORMED - Import: unl blob json (after base64 decoding) wrong
        // required field (validators) and/or types
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3"
                "JzIjoid3JvbmcifQ==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob validUntil <= validFrom
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
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
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob expired
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
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
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob not yet valid
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
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
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: depth > 32
        // DA: Impossible test

        // temMALFORMED - Import: !proof->isObject() && !proof->isArray()
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::proof] = "not object";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: return false

        // temMALFORMED - Import : xpop proof did not contain the specified txn
        // hash
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::proof][jss::children]["D"]
                   [jss::children]["7"][jss::hash] =
                       "12D47E7D543E15F1EDBA91CDF335722727851BDDA8C2FF8924772AD"
                       "C6B522A29";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: depth > 32
        // DA: Impossible test

        // temMALFORMED - Import: !proof.isObject() && !proof.isArray()
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::proof] = "not a object";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: computed txroot does not match xpop txroot,
        // invalid xpop.
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::proof][jss::children]["3"]
                   [jss::hash] =
                       "22D47E7D543E15F1EDBA91CDF335722727851BDDA8C2FF8924772AD"
                       "C6B522A29";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: error parsing coins | phash | acroot in the
        // ledger section of XPOP.
        // DA: Duplicate - syntaxCheckXPOP

        // temMALFORMED - Import: unl blob contained invalid validator entry,
        // skipping - not object
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3"
                "JzIjpbIndyb25nIiwid3JvbmciXX0=";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob contained invalid validator entry,
        // skipping - no manifest
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3"
                "JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1"
                "OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNE"
                "RFNUMifSx7InZhbGlkYXRpb25fcHVibGljX2tleSI6IkVEQkVFMzBGQUU5MkVF"
                "RTg4RTFDNDk4MEQwOUVDRkRFOTlBMTE2RDA3OEVDMjE4NTdEQjFCNDdCNDI2ND"
                "E4RTQyOCJ9XX0=";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob contained invalid validator entry,
        // skipping - wrong type validation_public_key
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3"
                "JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOjEsIm1hbmlmZXN0IjoiSkFB"
                "QUFBSnhJZTA0dlVSYS9XSVZsaURNR1d3bWFLSnJiN3Myc0puclZiT0tXTUVjRW"
                "dUZVhITWhBb0dNTmNzZ1VVTndLbytoN3poYVhLRitIR3c2WGhFam9ES3JhbGtZ"
                "bk1qS2RrY3dSUUloQUo3Y040SjZOZFZmcG52RUkvWmV1V0NUdm5wYUppcktORm"
                "NDM3pNT2pnd2pBaUFiS0kwZmJYZ1MxUkxsb05oeGRIaFZxOW96RVdWRTljSXdY"
                "RE4zQXhxeVkzQVNRQ3QwK3UvaU5TREQ2Ylh2VVR0ZG10NE5ydGxieDRWenVtVH"
                "BmalJZcDRsTW9JL2g0M3BVVGpwN1ZGb1hibktXalZocU5hR201NzdLNko2OTdY"
                "WjdUUUU9In0seyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOjEsIm1hbmlmZXN0Ij"
                "oiSkFBQUFBSnhJZTIrNHcrdWt1N29qaHhKZ05DZXo5NlpvUmJRZU93aGhYMnh0"
                "SHRDWkJqa0tITWhBOVNtSER4T2lDTVRabDVuUnhycDJ5aldaNWdqeDJEcm9VRn"
                "JVNG5yRXFVN2RrY3dSUUloQUxkRUZlalkrcFVuZ2xpN3NUdmliMEJtREhQN042"
                "aWtWRUJJNkg3SXdVMXpBaUJkc3lvU3FQY0MyTk1xZ0FuSFhIR2RrQUl3QlFEMU"
                "FVZzlYOFpKTHlmY3dIQVNRQ3QxYktWek9NeFJRbVIzd05LNGRLZG9mSUdyeEU5"
                "U2p1TFI2UGE4QjVuMDhTWUo4SzYyZ2UrOWE2QnRaYWxFbS9IT2RjejBOQUZPY3"
                "ljckYvQ3RTQTQ9In1dfQ==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob contained invalid validator entry,
        // skipping - wrong type
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3"
                "JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1"
                "OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNE"
                "RFNUMiLCJtYW5pZmVzdCI6MX0seyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJF"
                "REJFRTMwRkFFOTJFRUU4OEUxQzQ5ODBEMDlFQ0ZERTk5QTExNkQwNzhFQzIxOD"
                "U3REIxQjQ3QjQyNjQxOEU0MjgiLCJtYW5pZmVzdCI6MX1dfQ==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob contained an invalid validator key,
        // skipping - invalid format
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3"
                "JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRCIsIm1hbmlmZXN0Ijoi"
                "SkFBQUFBSnhJZTA0dlVSYS9XSVZsaURNR1d3bWFLSnJiN3Myc0puclZiT0tXTU"
                "VjRWdUZVhITWhBb0dNTmNzZ1VVTndLbytoN3poYVhLRitIR3c2WGhFam9ES3Jh"
                "bGtZbk1qS2RrY3dSUUloQUo3Y040SjZOZFZmcG52RUkvWmV1V0NUdm5wYUppck"
                "tORmNDM3pNT2pnd2pBaUFiS0kwZmJYZ1MxUkxsb05oeGRIaFZxOW96RVdWRTlj"
                "SXdYRE4zQXhxeVkzQVNRQ3QwK3UvaU5TREQ2Ylh2VVR0ZG10NE5ydGxieDRWen"
                "VtVHBmalJZcDRsTW9JL2g0M3BVVGpwN1ZGb1hibktXalZocU5hR201NzdLNko2"
                "OTdYWjdUUUU9In0seyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRCIsIm1hbm"
                "lmZXN0IjoiSkFBQUFBSnhJZTIrNHcrdWt1N29qaHhKZ05DZXo5NlpvUmJRZU93"
                "aGhYMnh0SHRDWkJqa0tITWhBOVNtSER4T2lDTVRabDVuUnhycDJ5aldaNWdqeD"
                "JEcm9VRnJVNG5yRXFVN2RrY3dSUUloQUxkRUZlalkrcFVuZ2xpN3NUdmliMEJt"
                "REhQN042aWtWRUJJNkg3SXdVMXpBaUJkc3lvU3FQY0MyTk1xZ0FuSFhIR2RrQU"
                "l3QlFEMUFVZzlYOFpKTHlmY3dIQVNRQ3QxYktWek9NeFJRbVIzd05LNGRLZG9m"
                "SUdyeEU5U2p1TFI2UGE4QjVuMDhTWUo4SzYyZ2UrOWE2QnRaYWxFbS9IT2Rjej"
                "BOQUZPY3ljckYvQ3RTQTQ9In1dfQ==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob contained an invalid validator key,
        // skipping - missing
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
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
                "dFNBND0ifV19";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob contained an invalid manifest,
        // skipping
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3"
                "JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1"
                "OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNE"
                "RFNUMiLCJtYW5pZmVzdCI6Indyb25nIn0seyJ2YWxpZGF0aW9uX3B1YmxpY19r"
                "ZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2Qj"
                "A5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6Indyb25nIn1d"
                "fQ==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob list entry manifest master key did
        // not match master key, skipping
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3"
                "JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1"
                "OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNE"
                "RFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUyKzR3K3VrdTdvamh4SmdOQ2V6"
                "OTZab1JiUWVPd2hoWDJ4dEh0Q1pCamtLSE1oQTlTbUhEeE9pQ01UWmw1blJ4cn"
                "AyeWpXWjVnangyRHJvVUZyVTRuckVxVTdka2N3UlFJaEFMZEVGZWpZK3BVbmds"
                "aTdzVHZpYjBCbURIUDdONmlrVkVCSTZIN0l3VTF6QWlCZHN5b1NxUGNDMk5NcW"
                "dBbkhYSEdka0FJd0JRRDFBVWc5WDhaSkx5ZmN3SEFTUUN0MWJLVnpPTXhSUW1S"
                "M3dOSzRkS2RvZklHcnhFOVNqdUxSNlBhOEI1bjA4U1lKOEs2MmdlKzlhNkJ0Wm"
                "FsRW0vSE9kY3owTkFGT2N5Y3JGL0N0U0E0PSJ9LHsidmFsaWRhdGlvbl9wdWJs"
                "aWNfa2V5IjoiRURBRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMT"
                "ZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFB"
                "QUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQm"
                "prS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJF"
                "cVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQk"
                "k2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4"
                "WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUj"
                "ZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9D"
                "dFNBND0ifV19";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob list entry manifest signature
        // invalid, skipping
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3"
                "JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1"
                "OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNE"
                "RFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQVZ4SWUwNHZVUmEvV0lWbGlETUdXd21h"
                "S0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aG"
                "FYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBu"
                "dkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG"
                "9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJY"
                "dlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm"
                "5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJs"
                "aWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMT"
                "ZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFB"
                "QUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQm"
                "prS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJF"
                "cVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQk"
                "k2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4"
                "WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUj"
                "ZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9D"
                "dFNBND0ifV19";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: validator nodepub did not appear in
        // validator list but did appear in data section
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value valData;
            valData["n84QWAYxKUHacmyFTnzK4bvqVcUfr6RwtaNxCM2cJRY59UHmz1Fr"] =
                "22800000012600000015292C4C84F53A29EC0A36EDAB6C61510AD4F33846A1"
                "23D86FDAD30CBF175E217BA7B5394A5A761DA5C6B7A45D678DA85017208060"
                "24F4F741C65B2E44B005CA293120889A6BC5F1E179335E20384AB3C6D15019"
                "B94C4657B7529C72DDCE970A87E2EB9EE8EE12580ADFE6CF93B8672E4B289B"
                "BC732103FCA947A7F08B146457BEF95AF0CF7C3ABF0D09CD1DC02099F7185C"
                "37BB32807576473045022100BBBE6EDE0B2B61CD369E2188C8FBFACCB35CA2"
                "D166FD29D5E3D7B2195083E74302201FAA160136301A43E518B9424A0DA5DC"
                "1E7EF8B90DAE2FA87310047498514EB4";
            tmpXpop[jss::validation][jss::data] = valData;
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: validator nodepub key appears more than
        // once in data section
        // DA: Impossible - Cannot serialize duplicate json fields

        // temMALFORMED - Import: validation inside xpop was not valid hex
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value valData;
            valData["n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN"] =
                "not a hex";
            tmpXpop[jss::validation][jss::data] = valData;
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation message was not for computed ledger
        // hash
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
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
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation inside xpop was not signed with a
        // signing key we recognise
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
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
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation inside xpop was not correctly
        // signed
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
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
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation inside xpop was not able to be
        // parsed
        // DA: Catch All

        // temMALFORMED - Import: xpop inner txn did not contain a sequence
        // number or fee No Sequence
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "1200632200000000201B0000006C201D0000535968400000003B9ACA007321"
                "EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A9"
                "6C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF"
                "0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C"
                "5177C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }
        // No Fee
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "12006322000000002400000002201B0000006C201D000053597321EDA8D46E"
                "11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440"
                "549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4"
                "375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519"
                "560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }
        // Bad Fee
        // DA: Impossible - Cannot serialize negative number
    }

    void
    testInvalidPreclaim(FeatureBitset features)
    {
        testcase("import invalid preclaim");

        using namespace test::jtx;
        using namespace std::literals;

        // ----------------------------------------------------------------------
        // preclaim

        // temDISABLED
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(21337, keys),
                features - featureImport};

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                ter(temDISABLED));
        }

        // tefINTERNAL
        // during preclaim could not parse xpop, bailing.
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation] = {};
            // DA: Sanity Check - tefINTERNAL(preclaim)
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // tefINTERNAL
        // during preclaim could not find importSequence, bailing.
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "1200632200000000201B0000006C201D0000535968400000003B9ACA007321"
                "EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A9"
                "6C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF"
                "0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C"
                "5177C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            // DA: Sanity Check - tefINTERNAL(preclaim)
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // tefIMPORT_BLACKHOLED - SetRegularKey (w/seed) AccountZero
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            // Set Regular Key
            Json::Value jv;
            jv[jss::Account] = alice.human();
            const AccountID ACCOUNT_ZERO(0);
            jv["RegularKey"] = to_string(ACCOUNT_ZERO);
            jv[jss::TransactionType] = jss::SetRegularKey;
            env(jv, alice);

            // Disable Master Key
            env(fset(alice, asfDisableMaster), sig(alice));
            env.close();

            // Import with Master Key
            Json::Value tmpXpop =
                import::loadXpop(ImportTCSetRegularKey::w_seed);
            env(import::import(alice, tmpXpop),
                ter(tefIMPORT_BLACKHOLED),
                fee(feeDrops * 10),
                sig(alice));
            env.close();
        }

        // tefIMPORT_BLACKHOLED - SetRegularKey (w/seed) AccountOne
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            // Set Regular Key
            Json::Value jv;
            jv[jss::Account] = alice.human();
            const AccountID ACCOUNT_ONE(1);
            jv["RegularKey"] = to_string(ACCOUNT_ONE);
            jv[jss::TransactionType] = jss::SetRegularKey;
            env(jv, alice);

            // Disable Master Key
            env(fset(alice, asfDisableMaster), sig(alice));
            env.close();

            // Import with Master Key
            Json::Value tmpXpop =
                import::loadXpop(ImportTCSetRegularKey::w_seed);
            env(import::import(alice, tmpXpop),
                ter(tefIMPORT_BLACKHOLED),
                fee(feeDrops * 10),
                sig(alice));
            env.close();
        }

        // tefIMPORT_BLACKHOLED - SetRegularKey (w/seed) AccountTwo
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            // Set Regular Key
            Json::Value jv;
            jv[jss::Account] = alice.human();
            const AccountID ACCOUNT_TWO(2);
            jv["RegularKey"] = to_string(ACCOUNT_TWO);
            jv[jss::TransactionType] = jss::SetRegularKey;
            env(jv, alice);

            // Disable Master Key
            env(fset(alice, asfDisableMaster), sig(alice));
            env.close();

            // Import with Master Key
            Json::Value tmpXpop =
                import::loadXpop(ImportTCSetRegularKey::w_seed);
            env(import::import(alice, tmpXpop),
                ter(tefIMPORT_BLACKHOLED),
                fee(feeDrops * 10),
                sig(alice));
            env.close();
        }

        // tefIMPORT_BLACKHOLED - SignersListSet (w/seed)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            // Set Regular Key
            Json::Value jv;
            jv[jss::Account] = alice.human();
            const AccountID ACCOUNT_ZERO(0);
            jv["RegularKey"] = to_string(ACCOUNT_ZERO);
            jv[jss::TransactionType] = jss::SetRegularKey;
            env(jv, alice);

            // Disable Master Key
            env(fset(alice, asfDisableMaster), sig(alice));
            env.close();

            // Import with Master Key
            Json::Value tmpXpop =
                import::loadXpop(ImportTCSignersListSet::w_seed);
            env(import::import(alice, tmpXpop),
                ter(tefIMPORT_BLACKHOLED),
                fee(feeDrops * 10),
                sig(alice));
            env.close();
        }

        // tefPAST_IMPORT_SEQ
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};
            auto const feeDrops = env.current()->fees().base;

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                fee(feeDrops * 10),
                ter(tesSUCCESS));
            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::min)),
                fee(feeDrops * 10),
                ter(tefPAST_IMPORT_SEQ));
        }

        // temBAD_FEE
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const alice = Account("alice");
            env.memoize(alice);
            Json::Value tx = import::import(
                alice, import::loadXpop(ImportTCAccountSet::w_seed));
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 10;
            env(tx, ter(temBAD_FEE));
        }

        // tefINTERNAL
        // during preclaim could not parse vlInfo, bailing.
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] = "YmFkSnNvbg==";
            // DA: Sanity Check - tefINTERNAL(preclaim)
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // tefPAST_IMPORT_VL_SEQ
        // import vl sequence already used, bailing.
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};
            auto const feeDrops = env.current()->fees().base;

            std::string const pkString =
                "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
                "CDC1";
            auto const pkHex = strUnHex(pkString);
            PublicKey const pk{makeSlice(*pkHex)};

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            Json::Value const xpop1_1 =
                import::loadXpop(ImportTCAccountSet::unl_seq_1_1);
            Json::Value const tx_1_1 = import::import(alice, xpop1_1);
            env(tx_1_1, fee(feeDrops * 10), ter(tesSUCCESS));

            BEAST_EXPECT(importVLSequence(env, pk) == 1);

            Json::Value const xpop2_1 =
                import::loadXpop(ImportTCAccountSet::unl_seq_2_1);
            Json::Value const tx_2_1 = import::import(bob, xpop2_1);
            env(tx_2_1, fee(feeDrops * 10), ter(tesSUCCESS));

            BEAST_EXPECT(importVLSequence(env, pk) == 2);

            Json::Value const xpop1_2 =
                import::loadXpop(ImportTCAccountSet::unl_seq_1_2);
            Json::Value const tx_1_2 = import::import(alice, xpop1_2);
            env(tx_1_2, fee(feeDrops * 10), ter(tefPAST_IMPORT_VL_SEQ));
        }

        // tesSUCCESS
        // DA: testImportSequence

        // tefINTERNAL
        // DA: Impossible Test

        // tefINTERNAL
        // DA: Impossible Test

        // tesSUCCESS
        // DA: testImportSequence

        // telIMPORT_VL_KEY_NOT_RECOGNISED
        // import vl key not recognized, bailing.
        {
            test::jtx::Env env{*this, network::makeNetworkConfig(21337)};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            Json::Value const tmpXpop =
                import::loadXpop(ImportTCAccountSet::w_seed);
            env(import::import(alice, tmpXpop),
                fee(feeDrops * 10),
                ter(telIMPORT_VL_KEY_NOT_RECOGNISED));
        }
    }

    void
    testInvalidDoApply(FeatureBitset features)
    {
        testcase("import invalid doApply");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, network::makeNetworkVLConfig(21337, keys)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");

        env.fund(XRP(1000), alice, bob);
        env.close();

        // burn 1000 xrp
        auto const master = Account("masterpassphrase");
        env(noop(master), fee(1'000'000'000), ter(tesSUCCESS));
        env.close();

        //----------------------------------------------------------------------
        // doApply

        // temDISABLED

        // tefINTERNAL/temMALFORMED - ctx_.tx.isFieldPresent(sfBlob)
        {
            Json::Value tx;
            tx[jss::TransactionType] = jss::Import;
            tx[jss::Account] = alice.human();
            env(tx, ter(temMALFORMED));
        }

        // tefINTERNAL/temMALFORMED - !xpop
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation] = {};  // one of many ways to throw error
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // tefINTERNAL/temMALFORMED
        // during apply could not find importSequence or fee, bailing.
        // No Fee
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::transaction][jss::blob] =
                "1200632200000000201B0000006C201D0000535968400000003B9ACA007321"
                "EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A9"
                "6C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF"
                "0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C"
                "5177C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }
        // No Sequence

        // tefINTERNAL
        // initBal <= beast::zero
        // Sanity Check

        // tefINTERNAL
        // ImportSequence passed
        // Sanity Check (tefPAST_IMPORT_SEQ)

        // tefINTERNAL/temMALFORMED
        // !infoVL
        {
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            tmpXpop[jss::validation][jss::unl][jss::blob] = "YmFkSnNvbg==";
            env(import::import(alice, tmpXpop), ter(temMALFORMED));
        }

        // tefINTERNAL
        // current > infoVL->first
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
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(alice);
            env.memoize(bob);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(bob, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, bob, ter(temMALFORMED));
            env.close();

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins);

            // confirm account does not exist
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle == nullptr);
        }

        // w/ seed -> dne
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(alice);
            env.memoize(bob);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            Json::Value tx = import::import(
                alice, import::loadXpop(ImportTCAccountSet::w_seed));
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - feeDrops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = XRP(zeroBurn ? 0 : 1000) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);

            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm account exists
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle != nullptr);
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ regular key other -> dne (bad signer)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.memoize(alice);
            env.memoize(bob);
            env.memoize(carol);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'990'000'000'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx - wrong regular key
            Json::Value txBad = import::import(
                alice, import::loadXpop(ImportTCAccountSet::w_regular_key));
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
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(alice);
            env.memoize(bob);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == 99'999'990'000'000'000);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            Json::Value tx = import::import(
                alice, import::loadXpop(ImportTCAccountSet::w_regular_key));
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, sig(bob), ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - feeDrops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = XRP(zeroBurn ? 0 : 1000) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

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

        // w/ signers list -> dne
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.memoize(alice);
            env.memoize(bob);
            env.memoize(carol);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCAccountSet::w_signers);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, msig(bob, carol), ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - feeDrops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 48) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);
            env(noop(alice),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tefNOT_MULTI_SIGNING));
            env.close();

            // alice cannnot sign
            env(noop(alice),
                sig(alice),
                fee(feeDrops),
                ter(tefMASTER_DISABLED));
        }

        // w/ seed -> funded
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins - (2 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                fee(10 * 10),
                ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - feeDrops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = XRP(zeroBurn ? 0 : 1000) - (feeDrops * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm account exists
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle != nullptr);
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ regular key -> funded
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins - (4 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCAccountSet::w_regular_key);
            Json::Value const tx = import::import(alice, xpopJson);
            env(tx, alice, sig(bob), fee(10 * 10), ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - feeDrops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = XRP(zeroBurn ? 0 : 1000) - (feeDrops * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

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
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins - (6 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCAccountSet::w_signers);
            Json::Value const tx = import::import(alice, xpopJson);
            env(tx,
                alice,
                msig(bob, carol),
                fee((3 * feeDrops) * 10),
                ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - feeDrops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn =
                drops(zeroBurn ? 0 : 48) - ((3 * feeDrops) * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);
            env(noop(alice),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tefNOT_MULTI_SIGNING));
            env.close();

            // alice cannnot sign
            env(noop(alice), sig(alice), fee(feeDrops), ter(tesSUCCESS));
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
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins - (2 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_flags)),
                fee(feeDrops * 10),
                ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 10) - (feeDrops * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

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
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm env
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(alice);
            env.memoize(bob);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + reward amount
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 12) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == bob.id());
            env(noop(alice), sig(bob), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ regular key -> dne
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm env
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.memoize(alice);
            env.memoize(bob);
            env.memoize(carol);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_regular_key);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, sig(bob), ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - initial value
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 12) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == carol.id());
            env(noop(alice), sig(carol), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ signers -> dne
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm env
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

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
            BEAST_EXPECT(preCoins == burnCoins);
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_signers);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, msig(bob, carol), ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + reward amount
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 48) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(
                acctSle && acctSle->isFieldPresent(sfRegularKey) &&
                acctSle->getAccountID(sfRegularKey) == dave.id());
            env(noop(alice), sig(dave), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ seed -> funded
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm env
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins - (4 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_seed);
            env(import::import(alice, xpopJson),
                fee(feeDrops * 10),
                ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - feeDrops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 12) - (feeDrops * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == bob.id());
            env(noop(alice), sig(bob), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ seed -> funded (update regular key)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == burnCoins - (6 * feeDrops));
            auto const envAlice = env.balance(alice);
            BEAST_EXPECT(envAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, carol));
            env(noop(alice), sig(carol), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == envCoins - (2 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == envAlice - (2 * feeDrops));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_seed);
            env(import::import(alice, xpopJson),
                fee(feeDrops * 10),
                sig(alice),
                ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 12) - (feeDrops * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm regular key set
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == bob.id());
            env(noop(alice), sig(bob), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ regular key -> funded (update regular key)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const dave = Account("dave");
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // confirm env
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == burnCoins - (8 * feeDrops));
            auto const envAlice = env.balance(alice);
            BEAST_EXPECT(envAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, dave));
            env(noop(alice), sig(dave), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == envCoins - (2 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == envAlice - (2 * feeDrops));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_regular_key);
            env(import::import(alice, xpopJson),
                fee(feeDrops * 10),
                sig(bob),
                ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 12) - (feeDrops * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == carol.id());
            env(noop(alice), sig(carol), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ signers list -> funded (update regular key)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const dave = Account("dave");
            auto const elsa = Account("elsa");
            env.fund(XRP(1000), alice, bob, carol, dave, elsa);
            env.close();

            // confirm env
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == burnCoins - (10 * feeDrops));
            auto const envAlice = env.balance(alice);
            BEAST_EXPECT(envAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, elsa));
            env(noop(alice), sig(elsa), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == envCoins - (2 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == envAlice - (2 * feeDrops));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_signers);
            env(import::import(alice, xpopJson),
                msig(bob, carol),
                fee((3 * feeDrops) * 10),
                ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn =
                drops(zeroBurn ? 0 : 48) - ((3 * feeDrops) * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == dave.id());
            env(noop(alice), sig(dave), fee(feeDrops), ter(tesSUCCESS));

            // confirm signers list not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);
        }

        // seed -> funded (empty regular key)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == burnCoins - (6 * feeDrops));
            auto const envAlice = env.balance(alice);
            BEAST_EXPECT(envAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, carol));
            env(noop(alice), sig(carol), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == envCoins - (2 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == envAlice - (2 * feeDrops));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_seed_empty);
            env(import::import(alice, xpopJson),
                fee(feeDrops * 10),
                sig(alice),
                ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 12) - (feeDrops * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(!acctSle->isFieldPresent(sfRegularKey));
        }

        // w/ regular key -> funded (empty regular key)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == burnCoins - (6 * feeDrops));
            auto const envAlice = env.balance(alice);
            BEAST_EXPECT(envAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, carol));
            env(noop(alice), sig(carol), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == envCoins - (2 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == envAlice - (2 * feeDrops));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_regular_key_empty);
            env(import::import(alice, xpopJson),
                fee(feeDrops * 10),
                sig(bob),
                ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 12) - (feeDrops * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

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
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const dave = Account("dave");
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // confirm env
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == burnCoins - (8 * feeDrops));
            auto const envAlice = env.balance(alice);
            BEAST_EXPECT(envAlice == XRP(1000));

            // set the regular key
            env(regkey(alice, dave));
            env(noop(alice), sig(dave), fee(feeDrops), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == envCoins - (2 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == envAlice - (2 * feeDrops));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_signers_empty);
            env(import::import(alice, xpopJson),
                msig(bob, carol),
                fee((3 * feeDrops) * 10),
                ter(tesSUCCESS));
            env.close();

            // total burn = burn drops - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn =
                drops(zeroBurn ? 0 : 48) - ((3 * feeDrops) * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

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
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(alice);
            env.memoize(bob);

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // confirm lsfPasswordSpent is set
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(
                (acctSle->getFieldU32(sfFlags) & lsfPasswordSpent) ==
                lsfPasswordSpent);
        }

        // funded -> set flag
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSetRegularKey::w_seed_zero);
            env(import::import(alice, xpopJson),
                fee(feeDrops * 10),
                ter(tesSUCCESS));
            env.close();

            // confirm lsfPasswordSpent is not set
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(
                (acctSle->getFieldU32(sfFlags) & lsfPasswordSpent) == 0);
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
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.memoize(alice);
            env.memoize(bob);
            env.memoize(carol);

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins);

            // confirm total coins header
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_seed_bad_fee);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            // tx[jss::Fee] = 0;
            env(tx, alice, ter(temBAD_FEE));
            env.close();
        }

        // w/ seed -> dne w/ seed
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.memoize(alice);

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins);

            // confirm alice balance
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = (burn drops + burn fee drops) - reward
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn =
                XRP(zeroBurn ? 0 : 2) + drops(zeroBurn ? 0 : 12) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm fee was minted
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

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
            BEAST_EXPECT(env.seq(alice) == aliceSeq + 1);

            // confirm noop master disabled
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ regular key -> dne
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const dave = Account("dave");
            env.memoize(alice);

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins);

            // confirm alice balance
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const burnAmt = XRP(2);
            auto const xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_regular_key);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, sig(bob), ter(tesSUCCESS));
            env.close();

            // total burn = (burn drops + burn fee drops) - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn =
                XRP(zeroBurn ? 0 : 2) + drops(zeroBurn ? 0 : 12) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm fee was minted
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm signers set
            auto const [signers, signersSle] =
                signersKeyAndSle(*env.current(), alice);
            auto const signerEntries =
                signersSle->getFieldArray(sfSignerEntries);
            BEAST_EXPECT(signerEntries.size() == 2);
            BEAST_EXPECT(signerEntries[0u].getFieldU16(sfSignerWeight) == 1);
            BEAST_EXPECT(
                signerEntries[0u].getAccountID(sfAccount) == dave.id());
            BEAST_EXPECT(signerEntries[1u].getFieldU16(sfSignerWeight) == 1);
            BEAST_EXPECT(
                signerEntries[1u].getAccountID(sfAccount) == carol.id());

            // confirm multisign tx
            env.close();
            auto const aliceSeq = env.seq(alice);
            env(noop(alice),
                msig(carol, dave),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(env.seq(alice) == aliceSeq + 1);

            // confirm regular key bad auth
            env(noop(alice), sig(bob), fee(feeDrops), ter(tefBAD_AUTH));

            // confirm noop master disabled
            env(noop(alice), fee(feeDrops), ter(tefMASTER_DISABLED));
        }

        // w/ signers -> dne
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const dave = Account("dave");
            auto const elsa = Account("elsa");
            env.memoize(alice);
            env.memoize(dave);
            env.memoize(elsa);

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins);

            // confirm alice balance
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_signers);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, msig(bob, carol), ter(tesSUCCESS));
            env.close();

            // confirm signers set
            auto const [signers, signersSle] =
                signersKeyAndSle(*env.current(), alice);
            auto const signerEntries =
                signersSle->getFieldArray(sfSignerEntries);
            BEAST_EXPECT(signerEntries.size() == 2);
            BEAST_EXPECT(signerEntries[0u].getFieldU16(sfSignerWeight) == 1);
            BEAST_EXPECT(
                signerEntries[0u].getAccountID(sfAccount) == dave.id());
            BEAST_EXPECT(signerEntries[1u].getFieldU16(sfSignerWeight) == 1);
            BEAST_EXPECT(
                signerEntries[1u].getAccountID(sfAccount) == elsa.id());

            // confirm multisign tx
            env.close();
            auto const aliceSeq = env.seq(alice);
            env(noop(alice),
                msig(dave, elsa),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(env.seq(alice) == aliceSeq + 1);

            // confirm regular key bad auth
            env(noop(alice), sig(bob), fee(feeDrops), ter(tefBAD_AUTH));

            // confirm noop master disabled
            env(noop(alice), fee(feeDrops), ter(tefMASTER_DISABLED));
        }

        // w/ seed -> funded
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == burnCoins - (6 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_seed);
            env(import::import(alice, xpopJson),
                fee(feeDrops * 10),
                ter(tesSUCCESS));
            env.close();

            // total burn = (burn drops + burn fee drops) - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = XRP(zeroBurn ? 0 : 2) +
                drops(zeroBurn ? 0 : 12) - (feeDrops * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm signers set
            auto const [signers, signersSle] =
                signersKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(
                signersSle && signersSle->isFieldPresent(sfSignerEntries));
            if (signersSle && signersSle->isFieldPresent(sfSignerEntries))
            {
                auto const signerEntries =
                    signersSle->getFieldArray(sfSignerEntries);
                BEAST_EXPECT(signerEntries.size() == 2);
                BEAST_EXPECT(
                    signerEntries[0u].getFieldU16(sfSignerWeight) == 1);
                BEAST_EXPECT(
                    signerEntries[0u].getAccountID(sfAccount) == carol.id());
                BEAST_EXPECT(
                    signerEntries[1u].getFieldU16(sfSignerWeight) == 1);
                BEAST_EXPECT(
                    signerEntries[1u].getAccountID(sfAccount) == bob.id());
            }

            // confirm multisign tx
            env.close();
            auto const aliceSeq = env.seq(alice);
            env(noop(alice),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(env.seq(alice) == aliceSeq + 1);

            // confirm noop alice tx
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ seed (empty) -> funded (has entries)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == burnCoins - (6 * feeDrops));
            auto const envAlice = env.balance(alice);
            BEAST_EXPECT(envAlice == XRP(1000));

            // set the signers list
            env(signers(alice, 2, {{bob, 1}, {carol, 1}}));
            env(noop(alice),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == envCoins - (4 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == envAlice - (4 * feeDrops));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_seed_empty);
            env(import::import(alice, xpopJson),
                fee(feeDrops * 10),
                ter(tesSUCCESS));
            env.close();

            // total burn = (burn drops + burn fee drops) - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = XRP(zeroBurn ? 0 : 2) +
                drops(zeroBurn ? 0 : 12) - (feeDrops * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);

            // confirm noop master success
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ regular key (empty) -> funded (has entries)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == burnCoins - (6 * feeDrops));
            auto const envAlice = env.balance(alice);
            BEAST_EXPECT(envAlice == XRP(1000));

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

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == envCoins - (6 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == envAlice - (6 * feeDrops));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_regular_key_empty);
            env(import::import(alice, xpopJson),
                fee(feeDrops * 10),
                sig(bob),
                ter(tesSUCCESS));
            env.close();

            // total burn = (burn drops + burn fee drops) - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = XRP(zeroBurn ? 0 : 2) +
                drops(zeroBurn ? 0 : 12) - (feeDrops * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm regular key
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == bob.id());
            env(noop(alice), sig(bob), fee(feeDrops), ter(tesSUCCESS));

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);

            // confirm noop master success
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ signers (empty) -> funded (has entries)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // confirm env
            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == burnCoins - (6 * feeDrops));
            auto const envAlice = env.balance(alice);
            BEAST_EXPECT(envAlice == XRP(1000));

            // set the signers list
            env(signers(alice, 2, {{bob, 1}, {carol, 1}}));
            env(noop(alice),
                msig(bob, carol),
                fee(3 * feeDrops),
                ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == envCoins - (4 * feeDrops));
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == envAlice - (4 * feeDrops));

            // import tx
            auto const xpopJson =
                import::loadXpop(ImportTCSignersListSet::w_signers_empty);
            env(import::import(alice, xpopJson),
                msig(bob, carol),
                fee((3 * feeDrops) * 10),
                ter(tesSUCCESS));
            env.close();

            // total burn = (burn drops + burn fee drops) - fee drops
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = XRP(zeroBurn ? 0 : 2) +
                drops(zeroBurn ? 0 : 48) - ((3 * feeDrops) * 10);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm signers not set
            auto const k = keylet::signers(alice);
            BEAST_EXPECT(env.current()->read(k) == nullptr);

            // confirm noop master success
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }
    }

    void
    testUsingTickets(FeatureBitset features)
    {
        testcase("using tickets");

        using namespace test::jtx;
        using namespace std::literals;

        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};
            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            std::uint32_t aliceTicketSeq{env.seq(alice) + 1};
            env(ticket::create(alice, 10));
            std::uint32_t const aliceSeq{env.seq(alice)};
            env.require(owners(alice, 10));

            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                fee(feeDrops * 10),
                ticket::use(aliceTicketSeq++),
                ter(tesSUCCESS));
            env.close();

            env.require(tickets(alice, env.seq(alice) - aliceTicketSeq));
            BEAST_EXPECT(env.seq(alice) == aliceSeq);
            env.require(owners(alice, 9));
        }
    }

    void
    testAccountIndex(FeatureBitset features)
    {
        testcase("account index");

        using namespace test::jtx;
        using namespace std::literals;

        // Account Index from Import
        {
            for (std::uint32_t const withFeature : {0, 1, 2})
            {
                auto const amend = withFeature == 0
                    ? features
                    : withFeature == 1 ? features - featureXahauGenesis
                                       : features - featureDeletableAccounts;
                test::jtx::Env env{
                    *this, network::makeNetworkVLConfig(21337, keys), amend};
                auto const feeDrops = env.current()->fees().base;

                // confirm total coins header
                auto const initCoins = env.current()->info().drops;
                BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

                // burn 10'000 xrp
                auto const master = Account("masterpassphrase");
                env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
                env.close();

                // confirm total coins header
                auto const burnCoins = env.current()->info().drops;
                BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

                auto const alice = Account("alice");
                env.fund(XRP(1000), alice);
                env.close();

                env(import::import(
                        alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                    fee(feeDrops * 10),
                    ter(tesSUCCESS));
                env.close();

                // confirm account index was set
                auto const [acct, acctSle] =
                    accountKeyAndSle(*env.current(), alice);

                // confirm sequence
                if (withFeature == 0)
                {
                    BEAST_EXPECT((*acctSle)[sfAccountIndex] == 0);
                }
                std::uint64_t const seq =
                    withFeature == 0 ? 12 : withFeature == 1 ? 6 : 12;
                BEAST_EXPECT((*acctSle)[sfSequence] == seq);

                // confirm account count was set
                if (withFeature == 0)
                {
                    auto const [fee, feeSle] = feesKeyAndSle(*env.current());
                    BEAST_EXPECT((*feeSle)[sfAccountCount] == 1);
                }
            }
        }

        // Account Index from Payment
        {
            for (std::uint32_t const withFeature : {0, 1, 2})
            {
                auto const amend = withFeature == 0
                    ? features
                    : withFeature == 1 ? features - featureXahauGenesis
                                       : features - featureDeletableAccounts;
                test::jtx::Env env{
                    *this, network::makeNetworkVLConfig(21337, keys), amend};
                env.close();

                auto const alice = Account("alice");
                auto const bob = Account("bob");
                auto const carol = Account("carol");
                env.fund(XRP(1000), alice, bob, carol);
                env.close();

                struct TestAccountData
                {
                    Account acct;
                    std::uint32_t index;
                    std::uint64_t sequence;
                };

                std::uint64_t const seq =
                    withFeature == 0 ? 11 : withFeature == 1 ? 5 : 11;
                std::array<TestAccountData, 3> acctTests = {{
                    {alice, 0, seq},
                    {bob, 1, seq},
                    {carol, 2, seq},
                }};

                for (auto const& t : acctTests)
                {
                    // confirm index was set
                    auto const [acct, acctSle] =
                        accountKeyAndSle(*env.current(), t.acct);

                    // confirm sequence
                    if (withFeature == 0)
                    {
                        BEAST_EXPECT((*acctSle)[sfAccountIndex] == t.index);
                    }
                    BEAST_EXPECT((*acctSle)[sfSequence] == t.sequence);
                }

                if (withFeature == 0)
                {
                    // confirm count was updated
                    auto const [fee, feeSle] = feesKeyAndSle(*env.current());
                    BEAST_EXPECT((*feeSle)[sfAccountCount] == 3);
                }
            }
        }
    }

    void
    testHookIssuer(FeatureBitset features)
    {
        testcase("hook issuer tx");

        using namespace test::jtx;
        using namespace std::literals;

        // Test that hook can reject and does NOT mint the funds.
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;
            bool const fixV2 = env.current()->rules().enabled(fixXahauV2);

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            auto const issuer = Account("issuer");
            env.fund(XRP(10000), alice, issuer);
            env.close();

            // fixXahauV2 - tshWEAK
            env(fset(issuer, asfTshCollect));
            env.close();

            std::string const createCodeHex =
                "0061736D01000000011C0460057F7F7F7F7F017E60037F7F7E017E60027F7F"
                "017F60017F017E02250303656E76057472616365000003656E7608726F6C6C"
                "6261636B000103656E76025F670002030201030503010002062B077F0141C0"
                "88040B7F004180080B7F0041BE080B7F004180080B7F0041C088040B7F0041"
                "000B7F0041010B07080104686F6F6B00030AC3800001BF800001017F230041"
                "106B220124002001200036020C41940841154180084114410010001A41AA08"
                "4114420A10011A41012200200010021A200141106A240042000B0B44010041"
                "80080B3D526F6C6C6261636B2E633A2043616C6C65642E0022526F6C6C6261"
                "636B2E633A2043616C6C65642E2200526F6C6C6261636B3A20526F6C6C6261"
                "636B21";
            std::string ns_str =
                "CAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECA"
                "FE";
            Json::Value jv = ripple::test::jtx::hook(
                issuer, {{hso(createCodeHex)}}, hsfOVERRIDE | hsfCOLLECT);
            jv[jss::Hooks][0U][jss::Hook][jss::HookNamespace] = ns_str;
            jv[jss::Hooks][0U][jss::Hook][jss::HookOn] =
                "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFDFFFFFFFFFFFFFFFFFFBFFF"
                "FF";
            env(jv, fee(1'000'000));
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preCoins = env.current()->info().drops;

            auto const result = fixV2 ? ter(tesSUCCESS) : ter(tecHOOK_REJECTED);
            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                import::issuer(issuer),
                fee(1'000'000),
                result);
            env.close();

            // fixXahauV2
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const mintXAH = fixV2 ? XRP(zeroBurn ? 0 : 1000) : XRP(0);
            // confirm fee was burned mint / no mint
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice - XRP(1) + mintXAH);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins - XRP(1) + mintXAH);

            // resubmit import without issuer
            auto const result2 =
                fixV2 ? ter(tefPAST_IMPORT_SEQ) : ter(tesSUCCESS);
            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                fee(feeDrops * 10),
                result2);
            env.close();

            // total burn
            auto const totalBurn =
                fixV2 ? XRP(0) : XRP(zeroBurn ? 0 : 1000) - (feeDrops * 10);

            // confirm fee was minted / not minted
            auto const postAlice2 = env.balance(alice);
            BEAST_EXPECT(postAlice2 == postAlice + totalBurn);

            // confirm total coins header
            auto const postCoins2 = env.current()->info().drops;
            BEAST_EXPECT(postCoins2 == postCoins + totalBurn);

            env.close();
        }
    }

    void
    testAccountDelete(FeatureBitset features)
    {
        testcase("account delete");

        using namespace test::jtx;
        using namespace std::literals;

        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            Json::Value const xpop =
                import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value const tx = import::import(alice, xpop);
            env(tx, fee(feeDrops * 10), ter(tesSUCCESS));

            // Close enough ledgers to be able to delete alices's account.
            incLgrSeqForAccDel(env, alice);

            // alice cannot delete account after import
            auto const acctDelFee{drops(env.current()->fees().increment)};
            env(acctdelete(alice, bob),
                fee(acctDelFee),
                ter(tecHAS_OBLIGATIONS));
        }
    }

    void
    testImportSequence(FeatureBitset features)
    {
        testcase("import sequence");

        using namespace test::jtx;
        using namespace std::literals;

        // test bad IMPORT_VL_KEYS no UNLReport
        {
            std::vector<std::string> const badVLKeys = {
                "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
                "CDC2"};
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, badVLKeys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            auto preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));
            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                fee(feeDrops * 10),
                ter(telIMPORT_VL_KEY_NOT_RECOGNISED));
            env.close();
        }

        // test good IMPORT_VL_KEYS no UNLReport
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            auto preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));
            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                fee(feeDrops * 10),
                ter(tesSUCCESS));
            env.close();
        }

        // test no IMPORT_VL_KEYS has UNLReport
        {
            test::jtx::Env env{*this, network::makeNetworkConfig(21337)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 10'000'000'000);

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            auto preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));

            // ADD UNL REPORT
            std::vector<std::string> const _ivlKeys = {
                "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
                "CDC1",
                "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
                "CDC2",
            };

            std::vector<PublicKey> ivlKeys;
            for (auto const& strPk : _ivlKeys)
            {
                auto pkHex = strUnHex(strPk);
                ivlKeys.emplace_back(makeSlice(*pkHex));
            }

            std::vector<std::string> const _vlKeys = {
                "ED8E43A943A174190BA2FAE91F44AC6E2D1D8202EFDCC2EA3DBB39814576D6"
                "90F7",
                "ED45D1840EE724BE327ABE9146503D5848EFD5F38B6D5FEDE71E80ACCE5E6E"
                "738B"};

            std::vector<PublicKey> vlKeys;
            for (auto const& strPk : _vlKeys)
            {
                auto pkHex = strUnHex(strPk);
                vlKeys.emplace_back(makeSlice(*pkHex));
            }

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = unl::createUNLReportTx(
                        env.current()->seq() + 1, ivlKeys[0], vlKeys[0]);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });

            // close the ledger
            env.close();

            BEAST_EXPECT(hasUNLReport(env) == true);

            // Test Import
            env(import::import(
                    alice, import::loadXpop(ImportTCAccountSet::w_seed)),
                fee(feeDrops * 10),
                ter(tesSUCCESS));
            env.close();
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
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(21337, keys),
                nullptr,
                beast::severities::kDisabled,
            };

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

            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            STAmount burnFee = XRP(zeroBurn ? 0 : 1000) + XRP(2);
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
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
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(21337, keys),
                nullptr,
                beast::severities::kDisabled,
            };

            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == 100'000'000'000'000'000);

            // burn all but 1,000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master),
                fee(envCoins - drops(1'000'000'000)),
                ter(tesSUCCESS));
            env.close();

            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == XRP(1000));

            auto const alice = Account("alice");
            env.memoize(alice);

            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            STAmount burnFee = XRP(zeroBurn ? 0 : 1000) + XRP(2);
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
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
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(21337, keys),
                nullptr,
                beast::severities::kDisabled,
            };

            auto const envCoins = env.current()->info().drops;
            BEAST_EXPECT(envCoins == 100'000'000'000'000'000);

            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == XRP(100'000'000'000));

            auto const alice = Account("alice");
            env.memoize(alice);

            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            STAmount burnFee = XRP(zeroBurn ? 0 : 1000) + XRP(2);
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
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
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::min);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + reward amount
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 10) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm account exists
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle != nullptr);
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ seed -> dne (min) (Reserve/Reference/Fee)
        // Owner Fee = 20 xrp
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(
                    21337, keys, "50", "10000000", "2000000")};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == initCoins - 10'000'000'000);

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::min);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + reward amount
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn = drops(zeroBurn ? 0 : 10) + XRP(20);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm account exists
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle != nullptr);
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }

        // w/ seed -> dne (max)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};

            auto const feeDrops = env.current()->fees().base;

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 99'999'998'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(99'999'998'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == initCoins - 99'999'998'000'000'000);

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::max);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + reward amount
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const totalBurn =
                drops(zeroBurn ? 0 : 99'999'939'799'000'000) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + totalBurn);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins + totalBurn);

            // confirm account exists
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle != nullptr);
            env(noop(alice), fee(feeDrops), ter(tesSUCCESS));
        }
    }

    void
    testHalving(FeatureBitset features)
    {
        testcase("halving");

        using namespace test::jtx;
        using namespace std::literals;

        // Halving @ ledger seq 1'999'999
        {
            test::jtx::Env env{
                *this,
                network::makeGenesisConfig(
                    features, 21337, keys, "10", "1000000", "200000", 1999998),
                features};

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 0);
            auto const initSeq = env.current()->info().seq;
            BEAST_EXPECT(initSeq == 1'999'999);

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + Init Reward
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const creditDrops = XRP(zeroBurn ? 0 : 1'000) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + creditDrops);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == initCoins + creditDrops);
        }

        // Halving @ ledger seq 2'000'000
        {
            test::jtx::Env env{
                *this,
                network::makeGenesisConfig(
                    features, 21337, keys, "10", "1000000", "200000", 1999998),
                features};

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 0);
            env.close();
            auto const initSeq = env.current()->info().seq;
            BEAST_EXPECT(initSeq == 2'000'000);

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + Init Reward
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const creditDrops = XRP(zeroBurn ? 0 : 1'000) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + creditDrops);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == initCoins + creditDrops);
        }

        // Halving @ ledger seq 2'000'001
        {
            test::jtx::Env env{
                *this,
                network::makeGenesisConfig(
                    features, 21337, keys, "10", "1000000", "200000", 1999998),
                features};

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 0);
            env.close();
            env.close();
            auto const initSeq = env.current()->info().seq;
            BEAST_EXPECT(initSeq == 2'000'001);

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + Init Reward
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const creditDrops = drops(zeroBurn ? 0 : 999999964) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + creditDrops);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == initCoins + creditDrops);
        }

        // Halving @ ledger seq 5'000'000
        {
            test::jtx::Env env{
                *this,
                network::makeGenesisConfig(
                    features, 21337, keys, "10", "1000000", "200000", 4999999),
                features};

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 0);
            auto const initSeq = env.current()->info().seq;
            BEAST_EXPECT(initSeq == 5'000'000);

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + Init Reward
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const creditDrops = drops(zeroBurn ? 0 : 892857142) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + creditDrops);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == initCoins + creditDrops);
        }

        // Halving @ ledger seq 20'000'000
        {
            test::jtx::Env env{
                *this,
                network::makeGenesisConfig(
                    features, 21337, keys, "10", "1000000", "200000", 19999999),
                features};

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 0);
            auto const initSeq = env.current()->info().seq;
            BEAST_EXPECT(initSeq == 20'000'000);

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + Init Reward
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const creditDrops = drops(zeroBurn ? 0 : 357142857) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + creditDrops);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == initCoins + creditDrops);
        }

        // Halving @ ledger seq 29'999'998
        {
            test::jtx::Env env{
                *this,
                network::makeGenesisConfig(
                    features, 21337, keys, "10", "1000000", "200000", 29999998),
                features};

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 0);
            auto const initSeq = env.current()->info().seq;
            BEAST_EXPECT(initSeq == 29'999'999);

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + Init Reward
            bool const zeroBurn =
                env.current()->rules().enabled(featureZeroB2M);
            auto const creditDrops = drops(zeroBurn ? 0 : 35) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + creditDrops);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == initCoins + creditDrops);
        }

        // Halving @ ledger seq 30'000'000
        {
            test::jtx::Env env{
                *this,
                network::makeGenesisConfig(
                    features, 21337, keys, "10", "1000000", "200000", 29999998),
                features};

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 0);
            env.close();
            auto const initSeq = env.current()->info().seq;
            BEAST_EXPECT(initSeq == 30'000'000);

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + Init Reward
            auto const creditDrops = drops(0) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + creditDrops);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == initCoins + creditDrops);
        }

        // Halving @ ledger seq 50'000'001
        {
            test::jtx::Env env{
                *this,
                network::makeGenesisConfig(
                    features, 21337, keys, "10", "1000000", "200000", 50000000),
                features};

            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 0);
            auto const initSeq = env.current()->info().seq;
            BEAST_EXPECT(initSeq == 50'000'001);

            // init env
            auto const alice = Account("alice");
            env.memoize(alice);

            // confirm env
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));

            // import tx
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            Json::Value tx = import::import(alice, xpopJson);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();

            // total burn = burn drops + Init Reward
            auto const creditDrops = drops(0) + XRP(2);

            // confirm fee was minted
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + creditDrops);

            // confirm total coins header
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == initCoins + creditDrops);
        }
    }

    void
    testBlackhole(FeatureBitset features)
    {
        testcase("blackhole");

        using namespace test::jtx;
        using namespace std::literals;

        auto blackholeAccount = [&](Env& env, Account const& acct) {
            // Set Regular Key
            Json::Value jv;
            jv[jss::Account] = acct.human();
            const AccountID ACCOUNT_ZERO(0);
            jv["RegularKey"] = to_string(ACCOUNT_ZERO);
            jv[jss::TransactionType] = jss::SetRegularKey;
            env(jv, acct);

            // Disable Master Key
            env(fset(acct, asfDisableMaster), sig(acct));
            env.close();
        };

        auto burnHeader = [&](Env& env) {
            // confirm total coins header
            auto const initCoins = env.current()->info().drops;
            BEAST_EXPECT(initCoins == 100'000'000'000'000'000);

            // burn 10'000 xrp
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(100'000'000'000'000), ter(tesSUCCESS));
            env.close();

            // confirm total coins header
            auto const burnCoins = env.current()->info().drops;
            BEAST_EXPECT(burnCoins == initCoins - 100'000'000'000'000);
        };

        // AccountSet (w/seed)
        {
            test::jtx::Env env{
                *this, network::makeNetworkVLConfig(21337, keys)};
            auto const feeDrops = env.current()->fees().base;

            // Burn Header
            burnHeader(env);

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            // Blackhole Account
            blackholeAccount(env, alice);

            // Import with Master Key
            Json::Value tmpXpop = import::loadXpop(ImportTCAccountSet::w_seed);
            env(import::import(alice, tmpXpop),
                ter(tesSUCCESS),
                fee(feeDrops * 10),
                sig(alice));
            env.close();
        }
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testWithFeats(all - fixXahauV2);
        testWithFeats(all - featureZeroB2M);
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
        testSyntaxCheckProofArray(features);
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
        testUsingTickets(features);
        testAccountIndex(features);
        testHookIssuer(features);
        testImportSequence(features);
        testAccountDelete(features);
        testMaxSupply(features);
        testMinMax(features);
        testHalving(features - featureOwnerPaysFee);
        testBlackhole(features);
    }
};

BEAST_DEFINE_TESTSUITE(Import, app, ripple);

}  // namespace test
}  // namespace ripple
