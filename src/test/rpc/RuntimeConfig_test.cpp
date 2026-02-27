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

#include <test/jtx.h>
#include <xrpld/app/misc/RuntimeConfig.h>
#include <xrpld/overlay/detail/TrafficCount.h>
#include <xrpl/protocol/jss.h>

namespace ripple {

class RuntimeConfig_test : public beast::unit_test::suite
{
    // Helper to call runtime_config RPC with JSON params
    Json::Value
    runtimeConfig(test::jtx::Env& env, Json::Value const& params)
    {
        return env.rpc(
            "json", "runtime_config", to_string(params))[jss::result];
    }

    // Helper to call runtime_config RPC with no params (GET)
    Json::Value
    runtimeConfig(test::jtx::Env& env)
    {
        return env.rpc("runtime_config")[jss::result];
    }

    void
    testGetEmpty()
    {
        testcase("GET empty config");
        using namespace test::jtx;
        Env env{*this};

        auto result = runtimeConfig(env);
        BEAST_EXPECT(result.isMember("configs"));
        BEAST_EXPECT(result["configs"].size() == 0);
        BEAST_EXPECT(!env.app().getRuntimeConfig().active());
    }

    void
    testSetGlobal()
    {
        testcase("SET global config");
        using namespace test::jtx;
        Env env{*this};

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["*"] = Json::objectValue;
        params["set"]["*"]["send_delay_ms"] = 100;
        params["set"]["*"]["send_delay_jitter_ms"] = 20;
        params["set"]["*"]["send_drop_pct"] = 5.5;

        auto result = runtimeConfig(env, params);
        BEAST_EXPECT(result.isMember("configs"));

        auto const& configs = result["configs"];
        if (!BEAST_EXPECT(configs.isMember("*")))
            return;

        auto const& global = configs["*"];
        BEAST_EXPECT(global["send_delay_ms"].asInt() == 100);
        BEAST_EXPECT(global["send_delay_jitter_ms"].asInt() == 20);
        BEAST_EXPECT(global["send_drop_pct"].asDouble() == 5.5);

        // Verify active state via RuntimeConfig directly
        BEAST_EXPECT(env.app().getRuntimeConfig().active());

        // Verify getConfig returns the global for any peer
        auto cfg = env.app().getRuntimeConfig().getConfig("10.0.0.1:51235");
        BEAST_EXPECT(cfg.has_value());
        BEAST_EXPECT(cfg->sendDelayMs == 100);
        BEAST_EXPECT(cfg->sendDelayJitterMs == 20);
        BEAST_EXPECT(cfg->sendDropPctX100 == 550);
    }

    void
    testSetPerPeer()
    {
        testcase("SET per-peer config with merge");
        using namespace test::jtx;
        Env env{*this};

        // Set global first
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["*"] = Json::objectValue;
            params["set"]["*"]["send_delay_ms"] = 100;
            params["set"]["*"]["send_drop_pct"] = 10.0;
            runtimeConfig(env, params);
        }

        // Set per-peer override (only delay, no drop)
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["10.0.0.2:51235"] = Json::objectValue;
            params["set"]["10.0.0.2:51235"]["send_delay_ms"] = 500;
            runtimeConfig(env, params);
        }

        auto& rc = env.app().getRuntimeConfig();

        // Per-peer should have merged values: delay from override, drop from *
        auto peerCfg = rc.getConfig("10.0.0.2:51235");
        if (!BEAST_EXPECT(peerCfg.has_value()))
            return;
        BEAST_EXPECT(peerCfg->sendDelayMs == 500);       // overridden
        BEAST_EXPECT(peerCfg->sendDropPctX100 == 1000);  // inherited from *

        // Other peers still get the global
        auto otherCfg = rc.getConfig("10.0.0.3:51235");
        if (!BEAST_EXPECT(otherCfg.has_value()))
            return;
        BEAST_EXPECT(otherCfg->sendDelayMs == 100);
        BEAST_EXPECT(otherCfg->sendDropPctX100 == 1000);
    }

    void
    testClear()
    {
        testcase("CLEAR specific target");
        using namespace test::jtx;
        Env env{*this};

        // Set global + per-peer
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["*"] = Json::objectValue;
            params["set"]["*"]["send_delay_ms"] = 50;
            params["set"]["10.0.0.2:51235"] = Json::objectValue;
            params["set"]["10.0.0.2:51235"]["send_delay_ms"] = 200;
            runtimeConfig(env, params);
        }

        // Clear per-peer
        {
            Json::Value params;
            params["clear"] = Json::arrayValue;
            params["clear"].append("10.0.0.2:51235");
            auto result = runtimeConfig(env, params);
            // Should still have "*"
            BEAST_EXPECT(result["configs"].isMember("*"));
            BEAST_EXPECT(!result["configs"].isMember("10.0.0.2:51235"));
        }

        // Per-peer now falls back to global
        auto cfg = env.app().getRuntimeConfig().getConfig("10.0.0.2:51235");
        BEAST_EXPECT(cfg.has_value());
        BEAST_EXPECT(cfg->sendDelayMs == 50);
    }

    void
    testClearAll()
    {
        testcase("CLEAR_ALL");
        using namespace test::jtx;
        Env env{*this};

        // Set some configs
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["*"] = Json::objectValue;
            params["set"]["*"]["send_delay_ms"] = 100;
            params["set"]["10.0.0.2:51235"] = Json::objectValue;
            params["set"]["10.0.0.2:51235"]["send_drop_pct"] = 50.0;
            runtimeConfig(env, params);
        }
        BEAST_EXPECT(env.app().getRuntimeConfig().active());

        // Clear all
        {
            Json::Value params;
            params["clear_all"] = true;
            auto result = runtimeConfig(env, params);
            BEAST_EXPECT(result["configs"].size() == 0);
        }
        BEAST_EXPECT(!env.app().getRuntimeConfig().active());
        BEAST_EXPECT(!env.app().getRuntimeConfig().getConfig("*").has_value());
    }

    void
    testPerPeerWithoutGlobal()
    {
        testcase("Per-peer config without global");
        using namespace test::jtx;
        Env env{*this};

        // Set only per-peer, no global
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["10.0.0.2:51235"] = Json::objectValue;
            params["set"]["10.0.0.2:51235"]["send_delay_ms"] = 300;
            runtimeConfig(env, params);
        }

        auto& rc = env.app().getRuntimeConfig();
        BEAST_EXPECT(rc.active());

        // Targeted peer gets the config
        auto peerCfg = rc.getConfig("10.0.0.2:51235");
        BEAST_EXPECT(peerCfg.has_value());
        BEAST_EXPECT(peerCfg->sendDelayMs == 300);

        // Other peers get nothing
        BEAST_EXPECT(!rc.getConfig("10.0.0.3:51235").has_value());
    }

    void
    testMessageTypeFilter()
    {
        testcase("Message type filter");
        using namespace test::jtx;
        Env env{*this};

        // Set with message_types filter
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["*"] = Json::objectValue;
            params["set"]["*"]["send_delay_ms"] = 100;
            params["set"]["*"]["message_types"] = Json::arrayValue;
            params["set"]["*"]["message_types"].append("proposal");
            params["set"]["*"]["message_types"].append("validation");
            auto result = runtimeConfig(env, params);

            // Verify response includes message_types
            auto const& global = result["configs"]["*"];
            BEAST_EXPECT(global.isMember("message_types"));
            BEAST_EXPECT(global["message_types"].size() == 2);
        }

        auto& rc = env.app().getRuntimeConfig();
        auto cfg = rc.getConfig("10.0.0.1:51235");
        if (!BEAST_EXPECT(cfg.has_value()))
            return;

        // Applies to proposal and validation categories
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::validation));

        // Does NOT apply to other categories
        BEAST_EXPECT(!cfg->appliesTo(TrafficCount::category::transaction));
        BEAST_EXPECT(!cfg->appliesTo(TrafficCount::category::base));
    }

    void
    testMessageTypeFilterEmpty()
    {
        testcase("No message type filter means all");
        using namespace test::jtx;
        Env env{*this};

        // Set without message_types — applies to all
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["*"] = Json::objectValue;
            params["set"]["*"]["send_delay_ms"] = 100;
            runtimeConfig(env, params);
        }

        auto cfg = env.app().getRuntimeConfig().getConfig("*");
        if (!BEAST_EXPECT(cfg.has_value()))
            return;

        BEAST_EXPECT(!cfg->messageCategories.has_value());
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::validation));
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::transaction));
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::base));
    }

    void
    testInvalidMessageType()
    {
        testcase("Invalid message type returns error");
        using namespace test::jtx;
        Env env{*this};

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["*"] = Json::objectValue;
        params["set"]["*"]["send_delay_ms"] = 100;
        params["set"]["*"]["message_types"] = Json::arrayValue;
        params["set"]["*"]["message_types"].append("proposals");  // typo
        auto result = runtimeConfig(env, params);

        BEAST_EXPECT(result.isMember("error"));
        BEAST_EXPECT(result["error"].asString() == "invalidParams");
        // Config should NOT have been applied
        BEAST_EXPECT(!env.app().getRuntimeConfig().active());
    }

    void
    testDropPctClamping()
    {
        testcase("send_drop_pct clamped to 0-100");
        using namespace test::jtx;
        Env env{*this};

        // Over 100
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["*"] = Json::objectValue;
            params["set"]["*"]["send_drop_pct"] = 200.0;
            runtimeConfig(env, params);
        }
        auto cfg = env.app().getRuntimeConfig().getConfig("*");
        BEAST_EXPECT(cfg.has_value());
        BEAST_EXPECT(cfg->sendDropPctX100 == 10000);  // clamped to 100%

        // Negative
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["*"] = Json::objectValue;
            params["set"]["*"]["send_drop_pct"] = -50.0;
            runtimeConfig(env, params);
        }
        cfg = env.app().getRuntimeConfig().getConfig("*");
        BEAST_EXPECT(cfg.has_value());
        BEAST_EXPECT(cfg->sendDropPctX100 == 0);  // clamped to 0%
    }

    void
    testPerPeerClearInheritedFilter()
    {
        testcase("Per-peer can override global filter to all");
        using namespace test::jtx;
        Env env{*this};

        // Global: only proposals
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["*"] = Json::objectValue;
            params["set"]["*"]["send_delay_ms"] = 100;
            params["set"]["*"]["message_types"] = Json::arrayValue;
            params["set"]["*"]["message_types"].append("proposal");
            runtimeConfig(env, params);
        }

        // Per-peer: message_types = [] (explicitly all)
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["10.0.0.2:51235"] = Json::objectValue;
            params["set"]["10.0.0.2:51235"]["message_types"] = Json::arrayValue;
            runtimeConfig(env, params);
        }

        auto& rc = env.app().getRuntimeConfig();

        // Per-peer should apply to all categories (empty set override)
        auto peerCfg = rc.getConfig("10.0.0.2:51235");
        BEAST_EXPECT(peerCfg.has_value());
        BEAST_EXPECT(peerCfg->appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(peerCfg->appliesTo(TrafficCount::category::validation));
        BEAST_EXPECT(peerCfg->appliesTo(TrafficCount::category::transaction));

        // Other peers still only get proposal filter from global
        auto otherCfg = rc.getConfig("10.0.0.3:51235");
        BEAST_EXPECT(otherCfg.has_value());
        BEAST_EXPECT(otherCfg->appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(!otherCfg->appliesTo(TrafficCount::category::validation));
    }

public:
    void
    run() override
    {
        testGetEmpty();
        testSetGlobal();
        testSetPerPeer();
        testClear();
        testClearAll();
        testPerPeerWithoutGlobal();
        testMessageTypeFilter();
        testMessageTypeFilterEmpty();
        testInvalidMessageType();
        testDropPctClamping();
        testPerPeerClearInheritedFilter();
    }
};

BEAST_DEFINE_TESTSUITE(RuntimeConfig, rpc, ripple);

}  // namespace ripple
