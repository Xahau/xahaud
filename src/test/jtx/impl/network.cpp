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

#include <ripple/basics/StringUtilities.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/app/Import_json.h>
#include <test/jtx/network.h>

namespace ripple {
namespace test {
namespace jtx {

namespace network {

std::unique_ptr<Config>
makeNetworkConfig(
    uint32_t networkID,
    std::string fee,
    std::string a_res,
    std::string o_res)
{
    using namespace jtx;
    return envconfig([&](std::unique_ptr<Config> cfg) {
        cfg->NETWORK_ID = networkID;
        Section config;
        config.append(
            {"reference_fee = " + fee,
             "account_reserve = " + a_res,
             "owner_reserve = " + o_res});
        auto setup = setup_FeeVote(config);
        cfg->FEES = setup;
        return cfg;
    });
}

std::unique_ptr<Config>
makeNetworkVLConfig(
    uint32_t networkID,
    std::vector<std::string> keys,
    std::string fee,
    std::string a_res,
    std::string o_res)
{
    using namespace jtx;
    return envconfig([&](std::unique_ptr<Config> cfg) {
        cfg->NETWORK_ID = networkID;
        Section config;
        config.append(
            {"reference_fee = " + fee,
             "account_reserve = " + a_res,
             "owner_reserve = " + o_res});
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
                    "Import VL Key '" + strPk + "' was not a valid key type.");

            cfg->IMPORT_VL_KEYS.emplace(strPk, makeSlice(*pkHex));
        }
        return cfg;
    });
}

std::unique_ptr<Config>
makeGenesisConfig(
    FeatureBitset features,
    uint32_t networkID,
    std::vector<std::string> keys,
    std::string fee,
    std::string a_res,
    std::string o_res,
    uint32_t ledgerID)
{
    using namespace jtx;

    Json::Value jsonValue;
    Json::Reader reader;
    reader.parse(ImportTCHalving::base_genesis, jsonValue);

    foreachFeature(features, [&](uint256 const& feature) {
        std::string featureName = featureToName(feature);
        std::optional<uint256> featureHash = getRegisteredFeature(featureName);
        if (featureHash.has_value())
        {
            std::string hashString = to_string(featureHash.value());
            jsonValue["ledger"]["accountState"][1]["Amendments"].append(
                hashString);
        }
    });

    jsonValue["ledger_current_index"] = ledgerID;
    jsonValue["ledger"]["ledger_index"] = to_string(ledgerID);
    jsonValue["ledger"]["seqNum"] = to_string(ledgerID);

    return envconfig([&](std::unique_ptr<Config> cfg) {
        cfg->NETWORK_ID = networkID;
        cfg->START_LEDGER = jsonValue.toStyledString();
        cfg->START_UP = Config::LOAD_JSON;
        Section config;
        config.append(
            {"reference_fee = " + fee,
             "account_reserve = " + a_res,
             "owner_reserve = " + o_res});
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
                    "Import VL Key '" + strPk + "' was not a valid key type.");

            cfg->IMPORT_VL_KEYS.emplace(strPk, makeSlice(*pkHex));
        }
        return cfg;
    });
}

}  // namespace network

}  // namespace jtx
}  // namespace test
}  // namespace ripple
