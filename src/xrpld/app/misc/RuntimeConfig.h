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

#include <xrpl/json/json_forwards.h>

#include <atomic>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ripple {

/** Peer-scoped runtime fault-injection values.

    Fields are optional so that a peer override can selectively inherit from
    peerDefaults.
*/
struct PeerFaultConfig
{
    std::optional<int> sendDelayMs;
    std::optional<int> sendDelayJitterMs;
    std::optional<int> sendDropPctX100;  // 0-10000 (pct * 100, avoids float)
    // If set (non-nullopt), only apply to these TrafficCount::category values.
    // nullopt = not specified (inherit from global on merge).
    // Empty set = explicitly "all categories" (overrides peerDefaults filter).
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
    PeerFaultConfig
    merged(PeerFaultConfig const& other) const
    {
        PeerFaultConfig result = *this;
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

/** Global runtime test controls.

    These values are daemon-wide.  They are intentionally not peer-targeted.
*/
struct ConsensusTestConfig
{
    std::optional<int> rngClaimDropPctX100;   // 0-10000 (pct * 100)
    std::optional<int> rngRevealDropPctX100;  // 0-10000 (pct * 100)
    // Bootstrap fast start: seed prevRoundTime_ to 3s instead of 15s on first
    // round, auto-disables after stable quorum is observed.
    std::optional<bool> bootstrapFastStart;
    // RNG poll interval in ms.  Controls how fast the heartbeat timer
    // ticks during RNG sub-state transitions.  Minimum 50ms.  Default 250ms.
    std::optional<int> rngPollMs;
    // Disable export signature attachment (testing sub-quorum scenarios).
    std::optional<bool> noExportSig;
    // Standalone-only entropy selection overrides for hook API tests.
    std::optional<int> standaloneEntropyTier;
    std::optional<int> standaloneEntropyCount;
    std::optional<int> standaloneEntropyDenominator;

    bool
    active() const
    {
        return (rngClaimDropPctX100 && *rngClaimDropPctX100 > 0) ||
            (rngRevealDropPctX100 && *rngRevealDropPctX100 > 0) ||
            (bootstrapFastStart && *bootstrapFastStart) || rngPollMs ||
            (noExportSig && *noExportSig) || standaloneEntropyTier ||
            standaloneEntropyCount || standaloneEntropyDenominator;
    }
};

/** Runtime test / fault-injection parameters.

    Public keys:
    - "global"         — daemon-wide consensus/export test knobs
    - "peer_defaults"  — default send fault config for all peers
    - "peer:ip:port"   — per-peer send fault override

    Peer keys match the outbound PeerImp remote endpoint used at send time.
    In x-testnet, n0->n2 resolves to peer:127.0.0.1:<n2 peer port> and affects
    only node 0's outbound sends to node 2. Reverse traffic and inbound
    ephemeral endpoints are separate unless configured separately.

    RPC handler: runtime_config (Role::ADMIN)
    Disconnect handler: disconnect (Role::ADMIN)

    External controls are compile-time gated by xahaud_runtime_test_config /
    XAHAUD_ENABLE_RUNTIME_TEST_CONFIG. Default builds ignore
    XAHAUD_RUNTIME_TEST_CONFIG and reject the runtime_config RPC.
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

    /** Look up effective peer fault config.

        Returns pre-merged result for peers with overrides, or peerDefaults for
        everyone else. Single map lookup, no merge on the read path.
    */
    std::optional<PeerFaultConfig>
    getPeerFaultConfig(std::string const& peerAddress) const;

    /** Return global consensus/export test config if set. */
    std::optional<ConsensusTestConfig>
    getConsensusTestConfig() const;

    /** Set daemon-wide consensus/export test config.

        Direct setters are for in-process tests and test harnesses. External
        env/RPC mutation is compile-gated; production code should not call
        these as a runtime configuration surface.
    */
    void
    setGlobalConfig(ConsensusTestConfig const& cfg);

    void
    clearGlobalConfig();

    /** Set default peer send fault config.

        Direct setters are for in-process tests and test harnesses. External
        env/RPC mutation is compile-gated; production code should not call
        these as a runtime configuration surface.
    */
    void
    setPeerDefaults(PeerFaultConfig const& cfg);

    void
    clearPeerDefaults();

    /** Set config for one peer key (without the "peer:" prefix).

        Direct setters are for in-process tests and test harnesses. External
        env/RPC mutation is compile-gated; production code should not call
        these as a runtime configuration surface.
    */
    void
    setPeerConfig(std::string const& peerAddress, PeerFaultConfig const& cfg);

    void
    clearPeerConfig(std::string const& peerAddress);

    /** Clear all configs. */
    void
    clearAllConfigs();

    /** Apply runtime_config RPC/env JSON and return current config or error. */
    Json::Value
    applyJson(Json::Value const& params);

    /** Return current config as RPC JSON. */
    Json::Value
    getJson() const;

private:
    void
    rebuildMerged();
    void
    updateActive();

    std::atomic<bool> active_{false};
    mutable std::shared_mutex mutex_;
    std::optional<ConsensusTestConfig> global_;
    std::optional<PeerFaultConfig> peerDefaults_;
    // Raw peer overrides as set by env vars / RPC.
    std::unordered_map<std::string, PeerFaultConfig> peers_;
    // Pre-merged entries for each peer (merged with peerDefaults_ at write
    // time)
    std::unordered_map<std::string, PeerFaultConfig> mergedPeers_;
};

/** Expand runtime-config message type names into TrafficCount categories.

    Names are intentionally string-based so test tools can target overlay
    traffic without depending on enum values.  Aliases may expand to several
    categories, for example candidate-set fetch covers the TMGetLedger request
    and TMLedgerData reply categories used by tx-set acquisition.
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
