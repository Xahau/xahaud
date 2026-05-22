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
#include <vector>

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
    std::optional<int> rngClaimDropPctX100;  // 0-10000 (pct * 100)
    // Controls explicit final proposal broadcast in the RNG reveal phase.
    // true  = attempt explicit-final proposal (experimental)
    // false = keep implicit mode (recommended default for production)
    //
    // NOTE: This knob is intentionally explicit opt-in. The consensus system
    // is fully functional without it via accept-time pseudo-tx injection.
    std::optional<bool> explicitFinalProposal;
    // Bootstrap fast start: seed prevRoundTime_ to 3s instead of 15s on first
    // round, auto-disables after stable quorum is observed.
    std::optional<bool> bootstrapFastStart;
    // RNG poll interval in ms.  Controls how fast the heartbeat timer
    // ticks during RNG sub-state transitions.  Minimum 50ms.  Default 250ms.
    std::optional<int> rngPollMs;
    // Disable export signature attachment (testing sub-quorum scenarios).
    std::optional<bool> noExportSig;
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
            (sendDropPctX100 && *sendDropPctX100 > 0) ||
            (rngClaimDropPctX100 && *rngClaimDropPctX100 > 0) ||
            explicitFinalProposal.has_value() ||
            bootstrapFastStart.has_value() || rngPollMs.has_value() ||
            noExportSig.has_value();
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
        if (other.rngClaimDropPctX100)
            result.rngClaimDropPctX100 = other.rngClaimDropPctX100;
        if (other.explicitFinalProposal.has_value())
            result.explicitFinalProposal = other.explicitFinalProposal;
        if (other.bootstrapFastStart.has_value())
            result.bootstrapFastStart = other.bootstrapFastStart;
        if (other.rngPollMs)
            result.rngPollMs = other.rngPollMs;
        if (other.noExportSig.has_value())
            result.noExportSig = other.noExportSig;
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

    RPC handler: runtime_config (Role::ADMIN)
    Disconnect handler: disconnect (Role::ADMIN)

    TODO: Consider gating RuntimeConfig activation on a config flag
    (e.g. [runtime_config] section in rippled.cfg) or compile-time
    define, so the system is fully inert on production nodes unless
    explicitly opted in.
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

/** Expand runtime-config message type names into TrafficCount categories.

    Names are intentionally string-based so test tools can target overlay
    traffic without depending on enum values.  Aliases may expand to several
    categories, for example candidate-set fetch covers the TMGetLedger request
    and TMLedgerData reply categories used by tx-set and sidecar acquisition.
*/
std::optional<std::set<std::size_t>>
runtimeConfigMessageCategoriesFromNames(
    std::vector<std::string> const& names,
    std::string& error);

/** Human-readable name for one runtime-config message category. */
std::string
runtimeConfigMessageCategoryName(std::size_t category);

}  // namespace ripple

#endif
