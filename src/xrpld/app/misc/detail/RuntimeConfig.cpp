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
#include <mutex>
#include <string>

namespace ripple {

namespace {
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

ConfigVals
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
    return cfg;
}
}  // namespace

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
                configs_[target] = parseConfigVals(root[target]);
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
