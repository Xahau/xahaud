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
#include <shared_mutex>

namespace ripple {

/** Global runtime test controls. These values are daemon-wide. */
struct ConsensusTestConfig
{
    std::optional<int> rngClaimDropPctX100;   // 0-10000 (pct * 100)
    std::optional<int> rngRevealDropPctX100;  // 0-10000 (pct * 100)
    // RNG poll interval in ms.  Controls how fast the heartbeat timer
    // ticks during RNG sub-state transitions.  Minimum 50ms.  Default 250ms.
    std::optional<int> rngPollMs;
    // Standalone-only entropy selection overrides for hook API tests.
    std::optional<int> standaloneEntropyTier;
    std::optional<int> standaloneEntropyCount;
    std::optional<int> standaloneEntropyDenominator;

    bool
    active() const
    {
        return (rngClaimDropPctX100 && *rngClaimDropPctX100 > 0) ||
            (rngRevealDropPctX100 && *rngRevealDropPctX100 > 0) || rngPollMs ||
            standaloneEntropyTier || standaloneEntropyCount ||
            standaloneEntropyDenominator;
    }
};

/** Runtime test parameters.

    Public key:
    - "global" — daemon-wide consensus test knobs

    RPC handler: runtime_config (Role::ADMIN)

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

    /** Return global consensus test config if set. */
    std::optional<ConsensusTestConfig>
    getConsensusTestConfig() const;

    /** Set daemon-wide consensus test config.

        Direct setters are for in-process tests and test harnesses. External
        env/RPC mutation is compile-gated; production code should not call
        these as a runtime configuration surface.
    */
    void
    setGlobalConfig(ConsensusTestConfig const& cfg);

    void
    clearGlobalConfig();

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
    updateActive();

    std::atomic<bool> active_{false};
    mutable std::shared_mutex mutex_;
    std::optional<ConsensusTestConfig> global_;
};

}  // namespace ripple

#endif
