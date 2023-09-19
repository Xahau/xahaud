//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2017 Ripple Labs Inc.
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

#include <ripple/app/misc/HashRouter.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include <ripple/basics/StringUtilities.h>
#include <string>
#include <test/jtx.h>
#include <vector>

namespace ripple {
namespace test {

struct UNLReport_test : public beast::unit_test::suite
{
    std::vector<PublicKey>
    createPublicKeys(std::size_t n)
    {
        std::vector<PublicKey> keys;
        std::size_t ss = 33;
        std::vector<uint8_t> data(ss, 0);
        data[0] = 0xED;
        for (int i = 0; i < n; ++i)
        {
            data[1]++;
            Slice s(data.data(), ss);
            keys.emplace_back(s);
        }
        return keys;
    }

    STTx
    createTx(LedgerIndex seq, PublicKey const& importKey, PublicKey const& valKey)
    {
        auto fill = [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.set(([&]() {
                auto inner = std::make_unique<STObject>(sfActiveValidator);
                inner->setFieldVL(sfPublicKey, valKey);
                return inner;
            })());
            obj.set(([&]() {
                auto inner = std::make_unique<STObject>(sfImportVLKey);
                inner->setFieldVL(sfPublicKey, importKey);
                return inner;
            })());
        };
        return STTx(ttUNL_REPORT, fill);
    }

    bool
    hasUNLReport(jtx::Env const& env)
    {
        auto const slep = env.le(keylet::UNLReport());
        return slep != nullptr;
    }

    void
    debugVLKeys(jtx::Env const& env)
    {
        auto const slep = env.le(keylet::UNLReport());
        auto const& vlKeys = slep->getFieldArray(sfImportVLKeys);
        std::cout << "VL KEYS ARRAY"<< "\n";
        for (auto const& k: vlKeys)
            std::cout << "vlKey: " << PublicKey(k[sfPublicKey]) << "\n";
    }

    bool
    validateImportVL(jtx::Env const& env, PublicKey const& pk)
    {
        auto const slep = env.le(keylet::UNLReport());
        auto const& vlKeys = slep->getFieldArray(sfImportVLKeys);
        for (auto const& k: vlKeys)
            if (PublicKey(k[sfPublicKey]) == pk)
                return true;
        return false;
    }

    bool
    validateActiveValidator(jtx::Env const& env, PublicKey const& pk)
    {
        auto const slep = env.le(keylet::UNLReport());
        auto const& activeVLs = slep->getFieldArray(sfActiveValidators);
        for (auto const& k: activeVLs)
            if (PublicKey(k[sfPublicKey]) == pk)
                return true;
        return false;
    }

    std::unique_ptr<Config>
    makeNetworkConfig(uint32_t networkID)
    {
        using namespace jtx;
        std::vector<std::string> const keys = {
            "ED74D4036C6591A4BDF9C54CEFA39B996A"
            "5DCE5F86D11FDA1874481CE9D5A1CDC1"};
        return envconfig([&](std::unique_ptr<Config> cfg) {
            cfg->NETWORK_ID = networkID;
            Section config;
            config.append(
                {"reference_fee = 10",
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

    void
    testDisabled(FeatureBitset features)
    {
        testcase("Test disabled");
        using namespace jtx;
        Env env{
            *this,
            envconfig(),
            features - featureXahauGenesis,
            nullptr};

        std::vector<PublicKey> publicKeys = createPublicKeys(3);

        auto l = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());

        l = std::make_shared<Ledger>(*l, env.app().timeKeeper().closeTime());

        // insert a ttUNL_REPORT pseudo into the open ledger
        env.app().openLedger().modify(
            [&](OpenView& view, beast::Journal j) -> bool {
                STTx tx = createTx(l->seq(), publicKeys[0], publicKeys[0]);
                uint256 txID = tx.getTransactionID();
                auto s = std::make_shared<ripple::Serializer>();
                tx.add(*s);
                env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                view.rawTxInsert(txID, std::move(s), nullptr);
                return true;
            });

        // close the ledger
        env.close();

        BEAST_EXPECT(hasUNLReport(env) == false);
    }

    void
    testNoImportVL(FeatureBitset features)
    {
        testcase("Test no import vl");
        using namespace jtx;

        std::vector<std::string> const _ivlKeys = {
            "ED74D4036C6591A4BDF9C54CEFA39B996A"
            "5DCE5F86D11FDA1874481CE9D5A1CDC1"};

        std::vector<PublicKey> ivlKeys;
        for (auto const& strPk : _ivlKeys)
        {
            auto pkHex = strUnHex(strPk);
            ivlKeys.emplace_back(makeSlice(*pkHex));
        }

        std::vector<std::string> const _vlKeys = {
            "ED8E43A943A174190BA2FAE91F44AC6E2D1D8202EFDCC2EA3DBB39814576D690F7",
            "ED45D1840EE724BE327ABE9146503D5848EFD5F38B6D5FEDE71E80ACCE5E6E738B",
        };

        std::vector<PublicKey> vlKeys;
        for (auto const& strPk : _vlKeys)
        {
            auto pkHex = strUnHex(strPk);
            vlKeys.emplace_back(makeSlice(*pkHex));
        }
        
        // Create UNLReport
        {
            Env env{
            *this,
            envconfig(),
            features,
            nullptr};

            auto l = std::make_shared<Ledger>(
                create_genesis,
                env.app().config(),
                std::vector<uint256>{},
                env.app().getNodeFamily());

            l = std::make_shared<Ledger>(*l, env.app().timeKeeper().closeTime());

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = createTx(l->seq(), ivlKeys[0], vlKeys[0]);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });

            // close the ledger
            env.close();

            BEAST_EXPECT(validateImportVL(env, ivlKeys[0]) == true);
            BEAST_EXPECT(validateActiveValidator(env, vlKeys[0]) == true);
        }

        // Update UNLReport
        {
            Env env{
            *this,
            envconfig(),
            features,
            nullptr};

            auto l = std::make_shared<Ledger>(
                create_genesis,
                env.app().config(),
                std::vector<uint256>{},
                env.app().getNodeFamily());

            l = std::make_shared<Ledger>(*l, env.app().timeKeeper().closeTime());

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = createTx(l->seq(), ivlKeys[0], vlKeys[0]);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });

            // close the ledger
            env.close();

            BEAST_EXPECT(validateImportVL(env, ivlKeys[0]) == true);
            BEAST_EXPECT(validateActiveValidator(env, vlKeys[0]) == true);

            l = std::make_shared<Ledger>(*l, env.app().timeKeeper().closeTime());

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = createTx(l->seq(), ivlKeys[0], vlKeys[1]);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });

            // close the ledger
            env.close();

            BEAST_EXPECT(validateImportVL(env, ivlKeys[0]) == true);
            BEAST_EXPECT(validateActiveValidator(env, vlKeys[1]) == true);
        }
    }

    void
    testImportVL(FeatureBitset features)
    {
        testcase("Test import vl");
        using namespace jtx;

        std::vector<std::string> const _ivlKeys = {
            "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
            "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC2",
        };

        std::vector<PublicKey> ivlKeys;
        for (auto const& strPk : _ivlKeys)
        {
            auto pkHex = strUnHex(strPk);
            ivlKeys.emplace_back(makeSlice(*pkHex));
        }

        std::vector<std::string> const _vlKeys = {
            "ED8E43A943A174190BA2FAE91F44AC6E2D1D8202EFDCC2EA3DBB39814576D690F7",
            "ED45D1840EE724BE327ABE9146503D5848EFD5F38B6D5FEDE71E80ACCE5E6E738B"
        };

        std::vector<PublicKey> vlKeys;
        for (auto const& strPk : _vlKeys)
        {
            auto pkHex = strUnHex(strPk);
            vlKeys.emplace_back(makeSlice(*pkHex));
        }
        
        // telIMPORT_VL_KEY_NOT_RECOGNISED
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337), features, nullptr};

            auto l = std::make_shared<Ledger>(
                create_genesis,
                env.app().config(),
                std::vector<uint256>{},
                env.app().getNodeFamily());

            l = std::make_shared<Ledger>(*l, env.app().timeKeeper().closeTime());

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = createTx(l->seq(), ivlKeys[1], vlKeys[0]);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });

            BEAST_EXPECT(hasUNLReport(env) == false);
        }

        // SUCCESS
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337), features, nullptr};

            auto l = std::make_shared<Ledger>(
                create_genesis,
                env.app().config(),
                std::vector<uint256>{},
                env.app().getNodeFamily());

            l = std::make_shared<Ledger>(*l, env.app().timeKeeper().closeTime());

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = createTx(l->seq(), ivlKeys[0], vlKeys[0]);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });

            // close the ledger
            env.close();

            BEAST_EXPECT(validateImportVL(env, ivlKeys[0]) == true);
            BEAST_EXPECT(validateImportVL(env, ivlKeys[1]) == false);
            BEAST_EXPECT(validateActiveValidator(env, vlKeys[0]) == true);
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testDisabled(features);
        testNoImportVL(features);
        testImportVL(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(UNLReport, app, ripple);

}  // namespace test
}  // namespace ripple
