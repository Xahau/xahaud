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
#include <cctype>
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

std::optional<bool>
parseBoolEnv(char const* env)
{
    if (!env)
        return std::nullopt;

    std::string value{env};
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

    if (value == "1" || value == "true" || value == "yes" || value == "on")
        return true;
    if (value == "0" || value == "false" || value == "no" || value == "off")
        return false;

    return std::nullopt;
}

std::optional<ConfigVals>
parseConfigVals(Json::Value const& v)
{
    ConfigVals cfg;
    if (v.isMember("send_delay_ms"))
        cfg.sendDelayMs = v["send_delay_ms"].asInt();
    if (v.isMember("send_delay_jitter_ms"))
        cfg.sendDelayJitterMs = v["send_delay_jitter_ms"].asInt();
    if (v.isMember("send_drop_pct"))
        cfg.sendDropPctX100 =
            static_cast<int>(v["send_drop_pct"].asDouble() * 100);
    if (v.isMember("rng_claim_drop_pct"))
        cfg.rngClaimDropPctX100 =
            static_cast<int>(v["rng_claim_drop_pct"].asDouble() * 100);
    if (v.isMember("explicit_final_proposal"))
        cfg.explicitFinalProposal = v["explicit_final_proposal"].asBool();
    if (v.isMember("bootstrap_fast_start"))
        cfg.bootstrapFastStart = v["bootstrap_fast_start"].asBool();
    if (v.isMember("rng_poll_ms"))
        cfg.rngPollMs = std::max(50, v["rng_poll_ms"].asInt());
    if (v.isMember("no_export_sig"))
        cfg.noExportSig = v["no_export_sig"].asBool();
    if (v.isMember("message_types"))
    {
        if (!v["message_types"].isArray())
            return std::nullopt;

        std::vector<std::string> names;
        for (auto const& mt : v["message_types"])
            names.push_back(mt.asString());

        std::string error;
        auto cats = runtimeConfigMessageCategoriesFromNames(names, error);
        if (!cats)
            return std::nullopt;
        cfg.messageCategories = *cats;
    }
    return cfg;
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
    // XAHAU_RUNTIME_CONFIG takes precedence (full JSON config)
    if (auto const* env = std::getenv("XAHAU_RUNTIME_CONFIG"))
    {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(env, root) && root.isObject())
        {
            std::unique_lock lock(mutex_);
            for (auto const& target : root.getMemberNames())
            {
                if (auto cfg = parseConfigVals(root[target]))
                    configs_[target] = *cfg;
            }
            rebuildMerged();
            updateActive();
        }
        return;
    }

    // Fall back to individual env vars -> "*" entry
    ConfigVals global;
    if (auto const* env = std::getenv("XAHAU_SEND_DELAY_MS"))
        global.sendDelayMs = std::atoi(env);
    if (auto const* env = std::getenv("XAHAU_SEND_DELAY_JITTER_MS"))
        global.sendDelayJitterMs = std::atoi(env);
    if (auto const* env = std::getenv("XAHAU_SEND_DROP_PCT"))
        global.sendDropPctX100 = static_cast<int>(std::atof(env) * 100);
    if (auto const* env = std::getenv("XAHAU_RNG_CLAIM_DROP_PCT"))
        global.rngClaimDropPctX100 = static_cast<int>(std::atof(env) * 100);
    // Explicit-final proposal is intentionally opt-in and defaults to
    // implicit behavior when unset.
    if (auto parsed =
            parseBoolEnv(std::getenv("XAHAUD_EXPLICIT_FINAL_PROPOSAL")))
        global.explicitFinalProposal = *parsed;
    if (auto parsed = parseBoolEnv(std::getenv("XAHAUD_BOOTSTRAP_FAST_START")))
        global.bootstrapFastStart = *parsed;
    if (auto const* env = std::getenv("XAHAU_RNG_POLL_MS"))
        global.rngPollMs = std::max(50, std::atoi(env));
    if (auto parsed = parseBoolEnv(std::getenv("XAHAUD_NO_EXPORT_SIG")))
        global.noExportSig = *parsed;

    if (global.active())
    {
        std::unique_lock lock(mutex_);
        configs_["*"] = global;
        updateActive();
    }
}

std::optional<ConfigVals>
RuntimeConfig::getConfig(std::string const& peerAddress) const
{
    std::shared_lock lock(mutex_);

    // Pre-merged entry for this peer?
    if (auto it = merged_.find(peerAddress); it != merged_.end())
        return it->second;

    // Fall back to global "*" (no merge needed)
    if (auto it = configs_.find("*"); it != configs_.end())
        return it->second;

    return std::nullopt;
}

void
RuntimeConfig::setConfig(std::string const& target, ConfigVals const& cfg)
{
    std::unique_lock lock(mutex_);
    configs_[target] = cfg;
    rebuildMerged();
    updateActive();
}

void
RuntimeConfig::clearConfig(std::string const& target)
{
    std::unique_lock lock(mutex_);
    configs_.erase(target);
    merged_.erase(target);
    rebuildMerged();
    updateActive();
}

void
RuntimeConfig::clearAllConfigs()
{
    std::unique_lock lock(mutex_);
    configs_.clear();
    merged_.clear();
    active_.store(false, std::memory_order_relaxed);
}

std::unordered_map<std::string, ConfigVals>
RuntimeConfig::getAllConfigs() const
{
    std::shared_lock lock(mutex_);
    return configs_;
}

void
RuntimeConfig::rebuildMerged()
{
    // Called with mutex_ write-locked.
    // Rebuild pre-merged entries for every per-peer key.
    merged_.clear();
    auto globalIt = configs_.find("*");
    for (auto const& [target, cfg] : configs_)
    {
        if (target == "*")
            continue;
        if (globalIt != configs_.end())
            merged_[target] = globalIt->second.merged(cfg);
        else
            merged_[target] = cfg;
    }
}

void
RuntimeConfig::updateActive()
{
    // Called with mutex_ write-locked.
    // Check merged_ (effective per-peer) and configs_["*"] (global).
    bool any = false;
    if (auto it = configs_.find("*"); it != configs_.end())
        any = it->second.active();
    if (!any)
    {
        any = std::any_of(merged_.begin(), merged_.end(), [](auto const& p) {
            return p.second.active();
        });
    }
    active_.store(any, std::memory_order_relaxed);
}

}  // namespace ripple
