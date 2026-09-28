//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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

#include <xrpld/app/misc/RuntimeConfig.h>

#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ripple {

namespace {
using CategorySet = std::set<std::size_t>;

struct CategoryAlias
{
    char const* name;
    std::vector<std::size_t> categories;
};

namespace traffic_category {
// Keep RuntimeConfig independent from xrpld.overlay. These values mirror
// TrafficCount::category; RuntimeConfig_test compares aliases against the real
// enum so drift is caught without creating an app.misc -> overlay dependency.
enum : std::size_t {
    base = 0,
    cluster = 1,
    overlay = 2,
    manifests = 3,
    transaction = 4,
    proposal = 5,
    validation = 6,
    validatorlist = 7,
    get_set = 8,
    share_set = 9,
    ld_tsc_get = 10,
    ld_tsc_share = 11,
    ld_txn_get = 12,
    ld_txn_share = 13,
    ld_asn_get = 14,
    ld_asn_share = 15,
    ld_get = 16,
    ld_share = 17,
    gl_tsc_share = 18,
    gl_tsc_get = 19,
    gl_txn_share = 20,
    gl_txn_get = 21,
    gl_asn_share = 22,
    gl_asn_get = 23,
    gl_share = 24,
    gl_get = 25,
    share_hash_ledger = 26,
    get_hash_ledger = 27,
    share_hash_tx = 28,
    get_hash_tx = 29,
    share_hash_txnode = 30,
    get_hash_txnode = 31,
    share_hash_asnode = 32,
    get_hash_asnode = 33,
    share_cas_object = 34,
    get_cas_object = 35,
    share_fetch_pack = 36,
    get_fetch_pack = 37,
    get_transactions = 38,
    share_hash = 39,
    get_hash = 40,
    proof_path_request = 41,
    proof_path_response = 42,
    replay_delta_request = 43,
    replay_delta_response = 44,
    have_transactions = 45,
    requested_transactions = 46,
    unknown = 47,
};
}  // namespace traffic_category

std::vector<CategoryAlias> const&
categoryAliases()
{
    namespace C = traffic_category;
    static std::vector<CategoryAlias> const aliases = {
        {"base", {C::base}},
        {"cluster", {C::cluster}},
        {"overlay", {C::overlay}},
        {"proposal", {C::proposal}},
        {"validation", {C::validation}},
        {"transaction", {C::transaction}},
        {"manifests", {C::manifests}},
        {"validator_list", {C::validatorlist}},
        {"validatorlist", {C::validatorlist}},

        {"have_set", {C::get_set, C::share_set}},
        {"set_get", {C::get_set}},
        {"set_share", {C::share_set}},

        {"candidate_set_fetch",
         {C::gl_tsc_get, C::gl_tsc_share, C::ld_tsc_get, C::ld_tsc_share}},
        {"candidate_set_request", {C::gl_tsc_get}},
        {"candidate_set_reply", {C::ld_tsc_share}},

        {"ledger_data",
         {C::ld_tsc_get,
          C::ld_tsc_share,
          C::ld_txn_get,
          C::ld_txn_share,
          C::ld_asn_get,
          C::ld_asn_share,
          C::ld_get,
          C::ld_share}},
        {"ledger_data_tsc_get", {C::ld_tsc_get}},
        {"ledger_data_tsc_share", {C::ld_tsc_share}},
        {"ledger_data_txn_get", {C::ld_txn_get}},
        {"ledger_data_txn_share", {C::ld_txn_share}},
        {"ledger_data_asn_get", {C::ld_asn_get}},
        {"ledger_data_asn_share", {C::ld_asn_share}},
        {"ledger_data_get", {C::ld_get}},
        {"ledger_data_share", {C::ld_share}},

        {"get_ledger",
         {C::gl_tsc_get,
          C::gl_tsc_share,
          C::gl_txn_get,
          C::gl_txn_share,
          C::gl_asn_get,
          C::gl_asn_share,
          C::gl_get,
          C::gl_share}},
        {"get_ledger_tsc_get", {C::gl_tsc_get}},
        {"get_ledger_tsc_share", {C::gl_tsc_share}},
        {"get_ledger_txn_get", {C::gl_txn_get}},
        {"get_ledger_txn_share", {C::gl_txn_share}},
        {"get_ledger_asn_get", {C::gl_asn_get}},
        {"get_ledger_asn_share", {C::gl_asn_share}},
        {"get_ledger_get", {C::gl_get}},
        {"get_ledger_share", {C::gl_share}},

        {"get_object",
         {C::share_hash_ledger,
          C::get_hash_ledger,
          C::share_hash_tx,
          C::get_hash_tx,
          C::share_hash_txnode,
          C::get_hash_txnode,
          C::share_hash_asnode,
          C::get_hash_asnode,
          C::share_cas_object,
          C::get_cas_object,
          C::share_fetch_pack,
          C::get_fetch_pack,
          C::get_transactions,
          C::share_hash,
          C::get_hash}},
        {"get_object_fetch_pack", {C::share_fetch_pack, C::get_fetch_pack}},
        {"get_object_fetch_pack_get", {C::get_fetch_pack}},
        {"get_object_fetch_pack_share", {C::share_fetch_pack}},
        {"get_object_get", {C::get_hash}},
        {"get_object_share", {C::share_hash}},
        {"get_object_transactions", {C::get_transactions}},

        {"proof_path", {C::proof_path_request, C::proof_path_response}},
        {"proof_path_request", {C::proof_path_request}},
        {"proof_path_response", {C::proof_path_response}},
        {"replay_delta", {C::replay_delta_request, C::replay_delta_response}},
        {"replay_delta_request", {C::replay_delta_request}},
        {"replay_delta_response", {C::replay_delta_response}},
        {"have_transactions", {C::have_transactions}},
        {"requested_transactions", {C::requested_transactions}},
    };
    return aliases;
}

std::optional<CategorySet>
categoriesForName(std::string const& name)
{
    for (auto const& alias : categoryAliases())
    {
        if (name == alias.name)
            return CategorySet{
                alias.categories.begin(), alias.categories.end()};
    }

    if (!name.empty() &&
        std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isdigit(c);
        }))
    {
        try
        {
            auto const cat = static_cast<std::size_t>(std::stoull(name));
            if (cat <= traffic_category::unknown)
                return CategorySet{cat};
        }
        catch (std::exception const&)
        {
        }
    }

    return std::nullopt;
}

Json::Value
invalidParams(std::string const& message)
{
    Json::Value err{Json::objectValue};
    err["error"] = "invalidParams";
    err["error_message"] = message;
    return err;
}

#ifdef XAHAUD_ENABLE_RUNTIME_TEST_CONFIG

bool
isPeerField(std::string const& name)
{
    return name == "send_delay_ms" || name == "send_delay_jitter_ms" ||
        name == "send_drop_pct" || name == "message_types";
}

bool
isGlobalField(std::string const& name)
{
    return name == "rng_claim_drop_pct" || name == "bootstrap_fast_start" ||
        name == "rng_reveal_drop_pct" || name == "rng_poll_ms" ||
        name == "no_export_sig" || name == "no_export_sig_hash";
}

bool
parseInt(
    Json::Value const& value,
    std::string const& field,
    int& out,
    std::string& error)
{
    if (!value.isInt())
    {
        error = field + " must be an integer";
        return false;
    }
    out = value.asInt();
    return true;
}

bool
parseBool(
    Json::Value const& value,
    std::string const& field,
    bool& out,
    std::string& error)
{
    if (!value.isBool())
    {
        error = field + " must be a boolean";
        return false;
    }
    out = value.asBool();
    return true;
}

bool
parsePctX100(
    Json::Value const& value,
    std::string const& field,
    int& out,
    std::string& error)
{
    if (!value.isNumeric() || value.isBool())
    {
        error = field + " must be numeric";
        return false;
    }

    auto const pct = value.asDouble();
    if (pct < 0.0 || pct > 100.0)
    {
        error = field + " must be between 0 and 100";
        return false;
    }
    out = static_cast<int>(pct * 100);
    return true;
}

bool
parseMessageTypes(
    Json::Value const& value,
    std::optional<CategorySet>& out,
    std::string& error)
{
    if (!value.isArray())
    {
        error = "message_types must be an array";
        return false;
    }

    std::vector<std::string> names;
    for (auto const& mt : value)
    {
        if (!mt.isString())
        {
            error = "message_types entries must be strings";
            return false;
        }
        names.push_back(mt.asString());
    }

    auto cats = runtimeConfigMessageCategoriesFromNames(names, error);
    if (!cats)
        return false;
    out = *cats;
    return true;
}

std::optional<PeerFaultConfig>
parsePeerFaultConfig(Json::Value const& v, std::string& error)
{
    if (!v.isObject())
    {
        error = "peer config must be an object or null";
        return std::nullopt;
    }

    PeerFaultConfig cfg;
    for (auto const& name : v.getMemberNames())
    {
        if (isGlobalField(name))
        {
            error = name + " is global-only; use global";
            return std::nullopt;
        }
        if (!isPeerField(name))
        {
            error = "unknown peer config field: " + name;
            return std::nullopt;
        }

        if (name == "send_delay_ms")
        {
            int parsed = 0;
            if (!parseInt(v[name], name, parsed, error))
                return std::nullopt;
            cfg.sendDelayMs = parsed;
        }
        else if (name == "send_delay_jitter_ms")
        {
            int parsed = 0;
            if (!parseInt(v[name], name, parsed, error))
                return std::nullopt;
            cfg.sendDelayJitterMs = parsed;
        }
        else if (name == "send_drop_pct")
        {
            int parsed = 0;
            if (!parsePctX100(v[name], name, parsed, error))
                return std::nullopt;
            cfg.sendDropPctX100 = parsed;
        }
        else if (name == "message_types")
        {
            if (!parseMessageTypes(v[name], cfg.messageCategories, error))
                return std::nullopt;
        }
    }
    return cfg;
}

std::optional<ConsensusTestConfig>
parseConsensusTestConfig(Json::Value const& v, std::string& error)
{
    if (!v.isObject())
    {
        error = "global config must be an object or null";
        return std::nullopt;
    }

    ConsensusTestConfig cfg;
    for (auto const& name : v.getMemberNames())
    {
        if (isPeerField(name))
        {
            error =
                name + " is peer-scoped; use peer_defaults or peer:<ip:port>";
            return std::nullopt;
        }
        if (!isGlobalField(name))
        {
            error = "unknown global config field: " + name;
            return std::nullopt;
        }

        if (name == "rng_claim_drop_pct")
        {
            int parsed = 0;
            if (!parsePctX100(v[name], name, parsed, error))
                return std::nullopt;
            cfg.rngClaimDropPctX100 = parsed;
        }
        else if (name == "rng_reveal_drop_pct")
        {
            int parsed = 0;
            if (!parsePctX100(v[name], name, parsed, error))
                return std::nullopt;
            cfg.rngRevealDropPctX100 = parsed;
        }
        else if (name == "bootstrap_fast_start")
        {
            bool parsed = false;
            if (!parseBool(v[name], name, parsed, error))
                return std::nullopt;
            cfg.bootstrapFastStart = parsed;
        }
        else if (name == "rng_poll_ms")
        {
            int parsed = 0;
            if (!parseInt(v[name], name, parsed, error))
                return std::nullopt;
            cfg.rngPollMs = std::max(50, parsed);
        }
        else if (name == "no_export_sig")
        {
            bool parsed = false;
            if (!parseBool(v[name], name, parsed, error))
                return std::nullopt;
            cfg.noExportSig = parsed;
        }
        else if (name == "no_export_sig_hash")
        {
            bool parsed = false;
            if (!parseBool(v[name], name, parsed, error))
                return std::nullopt;
            cfg.noExportSigHash = parsed;
        }
    }
    return cfg;
}

enum class ConfigKeyKind { global, peerDefaults, peer };

struct ParsedConfigKey
{
    ConfigKeyKind kind;
    std::string peer;
};

std::optional<ParsedConfigKey>
parseConfigKey(std::string const& key, std::string& error)
{
    if (key == "global")
        return ParsedConfigKey{ConfigKeyKind::global, {}};
    if (key == "peer_defaults")
        return ParsedConfigKey{ConfigKeyKind::peerDefaults, {}};

    constexpr std::string_view prefix = "peer:";
    if (key.starts_with(prefix))
    {
        auto peer = key.substr(prefix.size());
        if (peer.empty())
        {
            error = "peer key must include an address after peer:";
            return std::nullopt;
        }
        return ParsedConfigKey{ConfigKeyKind::peer, std::move(peer)};
    }

    error = "unknown runtime_config key: " + key;
    return std::nullopt;
}

#endif

Json::Value
peerFaultConfigJson(PeerFaultConfig const& cfg)
{
    Json::Value entry{Json::objectValue};
    if (cfg.sendDelayMs)
        entry["send_delay_ms"] = *cfg.sendDelayMs;
    if (cfg.sendDelayJitterMs)
        entry["send_delay_jitter_ms"] = *cfg.sendDelayJitterMs;
    if (cfg.sendDropPctX100)
        entry["send_drop_pct"] = *cfg.sendDropPctX100 / 100.0;
    if (cfg.messageCategories)
    {
        Json::Value types{Json::arrayValue};
        for (auto cat : *cfg.messageCategories)
            types.append(runtimeConfigMessageCategoryName(cat));
        entry["message_types"] = types;
    }
    return entry;
}

Json::Value
consensusTestConfigJson(ConsensusTestConfig const& cfg)
{
    Json::Value entry{Json::objectValue};
    if (cfg.rngClaimDropPctX100)
        entry["rng_claim_drop_pct"] = *cfg.rngClaimDropPctX100 / 100.0;
    if (cfg.rngRevealDropPctX100)
        entry["rng_reveal_drop_pct"] = *cfg.rngRevealDropPctX100 / 100.0;
    if (cfg.bootstrapFastStart.has_value())
        entry["bootstrap_fast_start"] = *cfg.bootstrapFastStart;
    if (cfg.rngPollMs)
        entry["rng_poll_ms"] = *cfg.rngPollMs;
    if (cfg.noExportSig.has_value())
        entry["no_export_sig"] = *cfg.noExportSig;
    if (cfg.noExportSigHash.has_value())
        entry["no_export_sig_hash"] = *cfg.noExportSigHash;
    return entry;
}
}  // namespace

std::optional<std::set<std::size_t>>
runtimeConfigMessageCategoriesFromNames(
    std::vector<std::string> const& names,
    std::string& error)
{
    CategorySet result;
    for (auto const& name : names)
    {
        auto cats = categoriesForName(name);
        if (!cats)
        {
            error = "Unknown message_type: " + name;
            return std::nullopt;
        }
        result.insert(cats->begin(), cats->end());
    }
    return result;
}

std::string
runtimeConfigMessageCategoryName(std::size_t category)
{
    for (auto const& alias : categoryAliases())
    {
        if (alias.categories.size() == 1 &&
            alias.categories.front() == category)
            return alias.name;
    }
    return std::to_string(category);
}

RuntimeConfig::RuntimeConfig()
{
#ifdef XAHAUD_ENABLE_RUNTIME_TEST_CONFIG
    if (auto const* env = std::getenv("XAHAUD_RUNTIME_TEST_CONFIG"))
    {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(env, root) && root.isObject())
            (void)applyJson(root);
    }
#endif
}

std::optional<PeerFaultConfig>
RuntimeConfig::getPeerFaultConfig(std::string const& peerAddress) const
{
    std::shared_lock lock(mutex_);

    // Pre-merged entry for this peer?
    if (auto it = mergedPeers_.find(peerAddress); it != mergedPeers_.end())
        return it->second;

    // Fall back to peer defaults (no merge needed)
    if (peerDefaults_)
        return peerDefaults_;

    return std::nullopt;
}

std::optional<ConsensusTestConfig>
RuntimeConfig::getConsensusTestConfig() const
{
    std::shared_lock lock(mutex_);
    return global_;
}

void
RuntimeConfig::setGlobalConfig(ConsensusTestConfig const& cfg)
{
    std::unique_lock lock(mutex_);
    global_ = cfg;
    updateActive();
}

void
RuntimeConfig::clearGlobalConfig()
{
    std::unique_lock lock(mutex_);
    global_.reset();
    updateActive();
}

void
RuntimeConfig::setPeerDefaults(PeerFaultConfig const& cfg)
{
    std::unique_lock lock(mutex_);
    peerDefaults_ = cfg;
    rebuildMerged();
    updateActive();
}

void
RuntimeConfig::clearPeerDefaults()
{
    std::unique_lock lock(mutex_);
    peerDefaults_.reset();
    rebuildMerged();
    updateActive();
}

void
RuntimeConfig::setPeerConfig(
    std::string const& peerAddress,
    PeerFaultConfig const& cfg)
{
    std::unique_lock lock(mutex_);
    peers_[peerAddress] = cfg;
    rebuildMerged();
    updateActive();
}

void
RuntimeConfig::clearPeerConfig(std::string const& peerAddress)
{
    std::unique_lock lock(mutex_);
    peers_.erase(peerAddress);
    mergedPeers_.erase(peerAddress);
    rebuildMerged();
    updateActive();
}

void
RuntimeConfig::clearAllConfigs()
{
    std::unique_lock lock(mutex_);
    global_.reset();
    peerDefaults_.reset();
    peers_.clear();
    mergedPeers_.clear();
    active_.store(false, std::memory_order_relaxed);
}

Json::Value
RuntimeConfig::applyJson(Json::Value const& params)
{
#ifndef XAHAUD_ENABLE_RUNTIME_TEST_CONFIG
    (void)params;
    return invalidParams("runtime_config support is not compiled in");
#else
    if (!params.isObject())
        return invalidParams("runtime_config params must be an object");

    for (auto const& name : params.getMemberNames())
    {
        if (name != "set" && name != "clear_all" && name != "command" &&
            name != "api_version" && name != "jsonrpc" && name != "method" &&
            name != "id")
        {
            return invalidParams("unknown runtime_config field: " + name);
        }
    }

    struct Mutation
    {
        ParsedConfigKey key{ConfigKeyKind::global, {}};
        bool clear = false;
        std::optional<ConsensusTestConfig> global;
        std::optional<PeerFaultConfig> peer;
    };

    std::vector<Mutation> mutations;
    if (params.isMember("set"))
    {
        auto const& set = params["set"];
        if (!set.isObject())
            return invalidParams("set must be an object");

        for (auto const& keyName : set.getMemberNames())
        {
            std::string error;
            auto key = parseConfigKey(keyName, error);
            if (!key)
                return invalidParams(error);

            Mutation mutation;
            mutation.key = *key;
            auto const& value = set[keyName];
            if (value.isNull())
            {
                mutation.clear = true;
            }
            else if (key->kind == ConfigKeyKind::global)
            {
                auto cfg = parseConsensusTestConfig(value, error);
                if (!cfg)
                    return invalidParams(error);
                mutation.global = *cfg;
            }
            else
            {
                auto cfg = parsePeerFaultConfig(value, error);
                if (!cfg)
                    return invalidParams(error);
                mutation.peer = *cfg;
            }
            mutations.push_back(std::move(mutation));
        }
    }

    if (params.isMember("clear_all"))
    {
        if (!params["clear_all"].isBool())
            return invalidParams("clear_all must be a boolean");
        if (params["clear_all"].asBool())
            clearAllConfigs();
    }

    for (auto const& mutation : mutations)
    {
        switch (mutation.key.kind)
        {
            case ConfigKeyKind::global:
                if (mutation.clear)
                    clearGlobalConfig();
                else
                    setGlobalConfig(*mutation.global);
                break;
            case ConfigKeyKind::peerDefaults:
                if (mutation.clear)
                    clearPeerDefaults();
                else
                    setPeerDefaults(*mutation.peer);
                break;
            case ConfigKeyKind::peer:
                if (mutation.clear)
                    clearPeerConfig(mutation.key.peer);
                else
                    setPeerConfig(mutation.key.peer, *mutation.peer);
                break;
        }
    }

    return getJson();
#endif
}

Json::Value
RuntimeConfig::getJson() const
{
    Json::Value result{Json::objectValue};
    Json::Value configs{Json::objectValue};

    std::shared_lock lock(mutex_);
    if (global_)
        configs["global"] = consensusTestConfigJson(*global_);
    if (peerDefaults_)
        configs["peer_defaults"] = peerFaultConfigJson(*peerDefaults_);
    for (auto const& [peer, cfg] : peers_)
        configs["peer:" + peer] = peerFaultConfigJson(cfg);

    result["configs"] = std::move(configs);
    return result;
}

void
RuntimeConfig::rebuildMerged()
{
    // Called with mutex_ write-locked.
    // Rebuild pre-merged entries for every per-peer key.
    mergedPeers_.clear();
    for (auto const& [target, cfg] : peers_)
    {
        if (peerDefaults_)
            mergedPeers_[target] = peerDefaults_->merged(cfg);
        else
            mergedPeers_[target] = cfg;
    }
}

void
RuntimeConfig::updateActive()
{
    // Called with mutex_ write-locked.
    // Check global, peerDefaults_, and merged peer configs.
    bool any = global_ && global_->active();
    if (!any && peerDefaults_)
        any = peerDefaults_->active();
    if (!any)
    {
        any = std::any_of(
            mergedPeers_.begin(), mergedPeers_.end(), [](auto const& p) {
                return p.second.active();
            });
    }
    active_.store(any, std::memory_order_relaxed);
}

}  // namespace ripple
