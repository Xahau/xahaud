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
#include <mutex>
#include <optional>
#include <string>

namespace ripple {

namespace {

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
isGlobalField(std::string const& name)
{
    return name == "rng_claim_drop_pct" || name == "rng_reveal_drop_pct" ||
        name == "rng_poll_ms";
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
        else if (name == "rng_poll_ms")
        {
            int parsed = 0;
            if (!parseInt(v[name], name, parsed, error))
                return std::nullopt;
            cfg.rngPollMs = std::max(50, parsed);
        }
    }
    return cfg;
}

#endif

Json::Value
consensusTestConfigJson(ConsensusTestConfig const& cfg)
{
    Json::Value entry{Json::objectValue};
    if (cfg.rngClaimDropPctX100)
        entry["rng_claim_drop_pct"] = *cfg.rngClaimDropPctX100 / 100.0;
    if (cfg.rngRevealDropPctX100)
        entry["rng_reveal_drop_pct"] = *cfg.rngRevealDropPctX100 / 100.0;
    if (cfg.rngPollMs)
        entry["rng_poll_ms"] = *cfg.rngPollMs;
    return entry;
}
}  // namespace

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
RuntimeConfig::clearAllConfigs()
{
    std::unique_lock lock(mutex_);
    global_.reset();
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

    bool clearGlobal = false;
    std::optional<ConsensusTestConfig> global;
    if (params.isMember("set"))
    {
        auto const& set = params["set"];
        if (!set.isObject())
            return invalidParams("set must be an object");

        for (auto const& keyName : set.getMemberNames())
        {
            if (keyName != "global")
                return invalidParams("unknown runtime_config key: " + keyName);

            auto const& value = set[keyName];
            if (value.isNull())
            {
                clearGlobal = true;
                continue;
            }

            std::string error;
            auto cfg = parseConsensusTestConfig(value, error);
            if (!cfg)
                return invalidParams(error);
            global = *cfg;
        }
    }

    if (params.isMember("clear_all"))
    {
        if (!params["clear_all"].isBool())
            return invalidParams("clear_all must be a boolean");
        if (params["clear_all"].asBool())
            clearAllConfigs();
    }

    if (clearGlobal)
        clearGlobalConfig();
    else if (global)
        setGlobalConfig(*global);

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

    result["configs"] = std::move(configs);
    return result;
}

void
RuntimeConfig::updateActive()
{
    // Called with mutex_ write-locked.
    active_.store(global_ && global_->active(), std::memory_order_relaxed);
}

}  // namespace ripple
