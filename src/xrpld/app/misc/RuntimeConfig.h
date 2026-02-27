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

#ifndef RIPPLE_APP_MISC_RUNTIMECONFIG_H_INCLUDED
#define RIPPLE_APP_MISC_RUNTIMECONFIG_H_INCLUDED

#include <atomic>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace ripple {

/** Bundle of runtime-configurable values.

    Fields are optional so that per-peer entries can selectively
    override global ("*") defaults — unset fields inherit from "*".
*/
struct ConfigVals
{
    std::optional<int> sendDelayMs;
    std::optional<int> sendDelayJitterMs;
    std::optional<int> sendDropPctX100;  // 0-10000 (pct * 100, avoids float)
    // If set (non-nullopt), only apply to these TrafficCount::category values.
    // nullopt = not specified (inherit from global on merge).
    // Empty set = explicitly "all categories" (overrides global filter).
    std::optional<std::set<std::size_t>> messageCategories;

    /** Check if this config applies to a given message category. */
    bool
    appliesTo(std::size_t category) const
    {
        return !messageCategories || messageCategories->empty() ||
            messageCategories->count(category) > 0;
    }

    bool
    active() const
    {
        return (sendDelayMs && *sendDelayMs > 0) ||
            (sendDelayJitterMs && *sendDelayJitterMs > 0) ||
            (sendDropPctX100 && *sendDropPctX100 > 0);
    }

    /** Merge other on top of this — other's set fields override. */
    ConfigVals
    merged(ConfigVals const& other) const
    {
        ConfigVals result = *this;
        if (other.sendDelayMs)
            result.sendDelayMs = other.sendDelayMs;
        if (other.sendDelayJitterMs)
            result.sendDelayJitterMs = other.sendDelayJitterMs;
        if (other.sendDropPctX100)
            result.sendDropPctX100 = other.sendDropPctX100;
        if (other.messageCategories)
            result.messageCategories = other.messageCategories;
        return result;
    }
};

/** Runtime-configurable parameters.

    Holds a map of ConfigVals keyed by target:
    - "*"       — global default, applies to all peers
    - "ip:port" — per-peer override

    Per-peer entries are pre-merged with "*" at write time so the
    read path is a single map lookup with no merge.

    Configuration layers (each overrides the previous):
    1. Compile-time defaults (empty map)
    2. XAHAU_RUNTIME_CONFIG JSON env var, or individual XAHAU_SEND_* vars
    3. Admin RPC `runtime_config` for live changes
*/
class RuntimeConfig
{
public:
    RuntimeConfig();

    /** Fast-path check — skip mutex if no runtime config active. */
    bool
    active() const
    {
        return active_.load(std::memory_order_relaxed);
    }

    /** Look up effective config for a peer.  Returns pre-merged result
        for peers with overrides, or the "*" entry for everyone else.
        Single map lookup, no merge on the read path. */
    std::optional<ConfigVals>
    getConfig(std::string const& peerAddress) const;

    /** Set config for a target ("*" or "ip:port"). */
    void
    setConfig(std::string const& target, ConfigVals const& cfg);

    /** Clear config for a target. */
    void
    clearConfig(std::string const& target);

    /** Clear all configs. */
    void
    clearAllConfigs();

    /** Return raw (unmerged) configs for RPC GET. */
    std::unordered_map<std::string, ConfigVals>
    getAllConfigs() const;

private:
    void
    rebuildMerged();
    void
    updateActive();

    std::atomic<bool> active_{false};
    mutable std::shared_mutex mutex_;
    // Raw entries as set by env vars / RPC ("*" and "ip:port")
    std::unordered_map<std::string, ConfigVals> configs_;
    // Pre-merged entries for each ip:port (merged with "*" at write time)
    std::unordered_map<std::string, ConfigVals> merged_;
};

}  // namespace ripple

#endif
