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

#include <cstdlib>
#include <optional>
#include <string>

namespace ripple {

class RuntimeConfig_test : public beast::unit_test::suite
{
    class EnvVarGuard
    {
    public:
        EnvVarGuard(char const* name, char const* value) : name_(name)
        {
            if (auto const* old = std::getenv(name))
                old_ = old;
            setenv(name, value, 1);
        }

        ~EnvVarGuard()
        {
            if (old_)
                setenv(name_, old_->c_str(), 1);
            else
                unsetenv(name_);
        }

    private:
        char const* name_;
        std::optional<std::string> old_;
    };

    class EnvVarUnsetGuard
    {
    public:
        explicit EnvVarUnsetGuard(char const* name) : name_(name)
        {
            if (auto const* old = std::getenv(name))
                old_ = old;
            unsetenv(name);
        }

        ~EnvVarUnsetGuard()
        {
            if (old_)
                setenv(name_, old_->c_str(), 1);
            else
                unsetenv(name_);
        }

    private:
        char const* name_;
        std::optional<std::string> old_;
    };

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
    testConfigValsMergedDirect()
    {
        testcase("ConfigVals direct merge");

        ConfigVals global;
        global.sendDelayMs = 100;
        global.sendDelayJitterMs = 10;
        global.sendDropPctX100 = 250;
        global.rngClaimDropPctX100 = 750;
        global.bootstrapFastStart = false;
        global.rngPollMs = 250;
        global.noExportSig = false;
        global.messageCategories =
            std::set<std::size_t>{TrafficCount::category::proposal};

        ConfigVals peer;
        peer.sendDelayMs = 500;
        peer.sendDelayJitterMs = 25;
        peer.rngPollMs = 75;
        peer.noExportSig = true;
        peer.messageCategories = std::set<std::size_t>{};

        auto const merged = global.merged(peer);
        BEAST_EXPECT(merged.sendDelayMs == 500);
        BEAST_EXPECT(merged.sendDelayJitterMs == 25);
        BEAST_EXPECT(merged.sendDropPctX100 == 250);
        BEAST_EXPECT(merged.rngClaimDropPctX100 == 750);
        BEAST_EXPECT(merged.bootstrapFastStart.has_value());
        BEAST_EXPECT(*merged.bootstrapFastStart == false);
        BEAST_EXPECT(merged.rngPollMs == 75);
        BEAST_EXPECT(merged.noExportSig.has_value());
        BEAST_EXPECT(*merged.noExportSig == true);
        BEAST_EXPECT(merged.messageCategories.has_value());
        BEAST_EXPECT(merged.messageCategories->empty());
        BEAST_EXPECT(merged.appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(merged.appliesTo(TrafficCount::category::validation));

        ConfigVals inactive;
        inactive.sendDelayMs = 0;
        inactive.sendDelayJitterMs = 0;
        inactive.sendDropPctX100 = 0;
        inactive.rngClaimDropPctX100 = 0;
        BEAST_EXPECT(!inactive.active());
        inactive.bootstrapFastStart = false;
        BEAST_EXPECT(inactive.active());
    }

    void
    testRuntimeConfigDirectEffectiveView()
    {
        testcase("RuntimeConfig direct effective view");

        RuntimeConfig rc;
        rc.clearAllConfigs();

        ConfigVals global;
        global.sendDelayMs = 100;
        global.sendDropPctX100 = 1000;
        rc.setConfig("*", global);

        ConfigVals peer;
        peer.sendDelayMs = 500;
        rc.setConfig("10.0.0.2:51235", peer);

        auto peerCfg = rc.getConfig("10.0.0.2:51235");
        if (!BEAST_EXPECT(peerCfg.has_value()))
            return;
        BEAST_EXPECT(peerCfg->sendDelayMs == 500);
        BEAST_EXPECT(peerCfg->sendDropPctX100 == 1000);
        BEAST_EXPECT(rc.active());

        ConfigVals newGlobal;
        newGlobal.sendDropPctX100 = 2500;
        rc.setConfig("*", newGlobal);

        peerCfg = rc.getConfig("10.0.0.2:51235");
        if (!BEAST_EXPECT(peerCfg.has_value()))
            return;
        BEAST_EXPECT(peerCfg->sendDelayMs == 500);
        BEAST_EXPECT(peerCfg->sendDropPctX100 == 2500);

        auto otherCfg = rc.getConfig("10.0.0.3:51235");
        if (!BEAST_EXPECT(otherCfg.has_value()))
            return;
        BEAST_EXPECT(!otherCfg->sendDelayMs.has_value());
        BEAST_EXPECT(otherCfg->sendDropPctX100 == 2500);

        rc.clearConfig("*");
        peerCfg = rc.getConfig("10.0.0.2:51235");
        if (!BEAST_EXPECT(peerCfg.has_value()))
            return;
        BEAST_EXPECT(peerCfg->sendDelayMs == 500);
        BEAST_EXPECT(!peerCfg->sendDropPctX100.has_value());
        BEAST_EXPECT(!rc.getConfig("10.0.0.3:51235").has_value());

        rc.clearConfig("10.0.0.2:51235");
        BEAST_EXPECT(!rc.active());
        BEAST_EXPECT(!rc.getConfig("10.0.0.2:51235").has_value());
    }

    void
    testRuntimeConfigDirectInactiveEntries()
    {
        testcase("RuntimeConfig direct inactive entries");

        RuntimeConfig rc;
        rc.clearAllConfigs();

        ConfigVals global;
        global.sendDelayMs = 0;
        global.sendDropPctX100 = 0;
        rc.setConfig("*", global);

        BEAST_EXPECT(!rc.active());
        auto cfg = rc.getConfig("10.0.0.1:51235");
        if (!BEAST_EXPECT(cfg.has_value()))
            return;
        BEAST_EXPECT(!cfg->active());

        ConfigVals peer;
        peer.noExportSig = false;
        rc.setConfig("10.0.0.2:51235", peer);

        BEAST_EXPECT(rc.active());
        cfg = rc.getConfig("10.0.0.2:51235");
        if (!BEAST_EXPECT(cfg.has_value()))
            return;
        BEAST_EXPECT(cfg->noExportSig.has_value());
        BEAST_EXPECT(*cfg->noExportSig == false);

        rc.clearAllConfigs();
        BEAST_EXPECT(!rc.active());
        BEAST_EXPECT(!rc.getConfig("*").has_value());
    }

    void
    testRuntimeConfigDirectRawAndCategoryHelpers()
    {
        testcase("RuntimeConfig direct raw map and category helpers");

        std::string error;
        auto cats = runtimeConfigMessageCategoriesFromNames(
            {"proposal", "validation"}, error);
        BEAST_EXPECT(cats.has_value());
        if (cats)
        {
            BEAST_EXPECT(cats->count(TrafficCount::category::proposal) == 1);
            BEAST_EXPECT(cats->count(TrafficCount::category::validation) == 1);
        }
        BEAST_EXPECT(error.empty());

        cats = runtimeConfigMessageCategoriesFromNames(
            {"candidate_set_fetch"}, error);
        BEAST_EXPECT(cats.has_value());
        if (cats)
        {
            BEAST_EXPECT(cats->count(TrafficCount::category::gl_tsc_get) == 1);
            BEAST_EXPECT(
                cats->count(TrafficCount::category::gl_tsc_share) == 1);
            BEAST_EXPECT(cats->count(TrafficCount::category::ld_tsc_get) == 1);
            BEAST_EXPECT(
                cats->count(TrafficCount::category::ld_tsc_share) == 1);
        }

        auto const numeric = std::to_string(TrafficCount::category::proposal);
        cats = runtimeConfigMessageCategoriesFromNames({numeric}, error);
        BEAST_EXPECT(cats.has_value());
        if (cats)
            BEAST_EXPECT(cats->count(TrafficCount::category::proposal) == 1);

        cats = runtimeConfigMessageCategoriesFromNames({"not_real"}, error);
        BEAST_EXPECT(!cats);
        BEAST_EXPECT(error == "Unknown message_type: not_real");

        BEAST_EXPECT(
            runtimeConfigMessageCategoryName(
                TrafficCount::category::proposal) == "proposal");
        BEAST_EXPECT(
            runtimeConfigMessageCategoryName(
                TrafficCount::category::unknown + 100) ==
            std::to_string(TrafficCount::category::unknown + 100));

        RuntimeConfig rc;
        rc.clearAllConfigs();

        ConfigVals global;
        global.sendDelayMs = 10;
        rc.setConfig("*", global);

        ConfigVals peer;
        peer.bootstrapFastStart = true;
        rc.setConfig("10.0.0.4:51235", peer);

        auto raw = rc.getAllConfigs();
        BEAST_EXPECT(raw.size() == 2);
        BEAST_EXPECT(raw["*"].sendDelayMs == 10);
        BEAST_EXPECT(raw["10.0.0.4:51235"].bootstrapFastStart.has_value());
    }

    void
    testRuntimeConfigIndividualEnvVars()
    {
        testcase("RuntimeConfig individual env vars");

        EnvVarUnsetGuard runtimeJson{"XAHAU_RUNTIME_CONFIG"};
        EnvVarGuard sendDelay{"XAHAU_SEND_DELAY_MS", "12"};
        EnvVarGuard jitter{"XAHAU_SEND_DELAY_JITTER_MS", "3"};
        EnvVarGuard drop{"XAHAU_SEND_DROP_PCT", "4.5"};
        EnvVarGuard rngDrop{"XAHAU_RNG_CLAIM_DROP_PCT", "6.25"};
        EnvVarGuard bootstrap{"XAHAUD_BOOTSTRAP_FAST_START", "yes"};
        EnvVarGuard rngPoll{"XAHAU_RNG_POLL_MS", "5"};
        EnvVarGuard noExportSig{"XAHAUD_NO_EXPORT_SIG", "0"};

        RuntimeConfig rc;
        auto cfg = rc.getConfig("*");
        if (!BEAST_EXPECT(cfg.has_value()))
            return;

        BEAST_EXPECT(rc.active());
        BEAST_EXPECT(cfg->sendDelayMs == 12);
        BEAST_EXPECT(cfg->sendDelayJitterMs == 3);
        BEAST_EXPECT(cfg->sendDropPctX100 == 450);
        BEAST_EXPECT(cfg->rngClaimDropPctX100 == 625);
        BEAST_EXPECT(cfg->bootstrapFastStart.has_value());
        BEAST_EXPECT(*cfg->bootstrapFastStart == true);
        BEAST_EXPECT(cfg->rngPollMs == 50);
        BEAST_EXPECT(cfg->noExportSig.has_value());
        BEAST_EXPECT(*cfg->noExportSig == false);
    }

    void
    testRuntimeConfigJsonEnvMergesTargets()
    {
        testcase("RuntimeConfig JSON env config");

        EnvVarGuard runtimeJson{
            "XAHAU_RUNTIME_CONFIG",
            R"({"*":{"send_delay_ms":100,"send_delay_jitter_ms":20,)"
            R"("send_drop_pct":1.25,"rng_claim_drop_pct":3.5,)"
            R"("bootstrap_fast_start":false,)"
            R"("rng_poll_ms":5,"no_export_sig":true,)"
            R"("message_types":["proposal"]},)"
            R"("10.0.0.5:51235":{"send_delay_ms":200,)"
            R"("send_drop_pct":2.5,"rng_claim_drop_pct":4.5,)"
            R"("bootstrap_fast_start":true,)"
            R"("rng_poll_ms":125,"no_export_sig":false,)"
            R"("message_types":[]}})"};

        RuntimeConfig rc;
        auto global = rc.getConfig("*");
        if (!BEAST_EXPECT(global.has_value()))
            return;
        BEAST_EXPECT(global->sendDelayMs == 100);
        BEAST_EXPECT(global->sendDelayJitterMs == 20);
        BEAST_EXPECT(global->sendDropPctX100 == 125);
        BEAST_EXPECT(global->rngClaimDropPctX100 == 350);
        BEAST_EXPECT(global->bootstrapFastStart.has_value());
        BEAST_EXPECT(*global->bootstrapFastStart == false);
        BEAST_EXPECT(global->rngPollMs == 50);
        BEAST_EXPECT(global->noExportSig.has_value());
        BEAST_EXPECT(*global->noExportSig == true);
        BEAST_EXPECT(global->appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(!global->appliesTo(TrafficCount::category::validation));

        auto peer = rc.getConfig("10.0.0.5:51235");
        if (!BEAST_EXPECT(peer.has_value()))
            return;
        BEAST_EXPECT(peer->sendDelayMs == 200);
        BEAST_EXPECT(peer->sendDelayJitterMs == 20);
        BEAST_EXPECT(peer->sendDropPctX100 == 250);
        BEAST_EXPECT(peer->rngClaimDropPctX100 == 450);
        BEAST_EXPECT(peer->bootstrapFastStart.has_value());
        BEAST_EXPECT(*peer->bootstrapFastStart == true);
        BEAST_EXPECT(peer->rngPollMs == 125);
        BEAST_EXPECT(peer->noExportSig.has_value());
        BEAST_EXPECT(*peer->noExportSig == false);
        BEAST_EXPECT(peer->appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(peer->appliesTo(TrafficCount::category::validation));
    }

    void
    testRuntimeConfigInvalidJsonEnvIgnored()
    {
        testcase("Invalid XAHAU_RUNTIME_CONFIG JSON is ignored");

        EnvVarGuard runtimeJson{"XAHAU_RUNTIME_CONFIG", R"([])"};

        RuntimeConfig rc;
        BEAST_EXPECT(!rc.active());
        BEAST_EXPECT(!rc.getConfig("*").has_value());
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
    testMessageTypeAliases()
    {
        testcase("Message type aliases expand to multiple categories");
        using namespace test::jtx;
        Env env{*this};

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["*"] = Json::objectValue;
        params["set"]["*"]["send_delay_ms"] = 100;
        params["set"]["*"]["message_types"] = Json::arrayValue;
        params["set"]["*"]["message_types"].append("candidate_set_fetch");
        auto result = runtimeConfig(env, params);

        auto const& global = result["configs"]["*"];
        BEAST_EXPECT(global.isMember("message_types"));
        BEAST_EXPECT(global["message_types"].size() == 4);

        auto cfg = env.app().getRuntimeConfig().getConfig("*");
        if (!BEAST_EXPECT(cfg.has_value()))
            return;

        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::gl_tsc_get));
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::gl_tsc_share));
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::ld_tsc_get));
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::ld_tsc_share));
        BEAST_EXPECT(!cfg->appliesTo(TrafficCount::category::proposal));
    }

    void
    testEnvMessageTypeFilter()
    {
        testcase("XAHAU_RUNTIME_CONFIG parses message_types");

        EnvVarGuard guard{
            "XAHAU_RUNTIME_CONFIG",
            R"({"*":{"send_delay_ms":100,"message_types":["candidate_set_fetch"]}})"};

        RuntimeConfig rc;
        auto cfg = rc.getConfig("*");
        if (!BEAST_EXPECT(cfg.has_value()))
            return;

        BEAST_EXPECT(rc.active());
        BEAST_EXPECT(cfg->sendDelayMs == 100);
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::gl_tsc_get));
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::ld_tsc_share));
        BEAST_EXPECT(!cfg->appliesTo(TrafficCount::category::proposal));
    }

    void
    testInvalidEnvMessageType()
    {
        testcase("Invalid XAHAU_RUNTIME_CONFIG message_types are ignored");

        EnvVarGuard guard{
            "XAHAU_RUNTIME_CONFIG",
            R"({"*":{"send_delay_ms":100,"message_types":["not_a_category"]}})"};

        RuntimeConfig rc;
        BEAST_EXPECT(!rc.active());
        BEAST_EXPECT(!rc.getConfig("*").has_value());
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
    testInvalidMessageTypesShape()
    {
        testcase("Non-array message_types returns error");
        using namespace test::jtx;
        Env env{*this};

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["*"] = Json::objectValue;
        params["set"]["*"]["send_delay_ms"] = 100;
        params["set"]["*"]["message_types"] = "proposal";
        auto result = runtimeConfig(env, params);

        BEAST_EXPECT(result.isMember("error"));
        BEAST_EXPECT(result["error"].asString() == "invalidParams");
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
    testRngClaimDropPct()
    {
        testcase("rng_claim_drop_pct round-trips");
        using namespace test::jtx;
        Env env{*this};

        // Set rng_claim_drop_pct
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["*"] = Json::objectValue;
            params["set"]["*"]["rng_claim_drop_pct"] = 50.0;
            auto result = runtimeConfig(env, params);

            auto const& global = result["configs"]["*"];
            BEAST_EXPECT(global["rng_claim_drop_pct"].asDouble() == 50.0);
        }

        BEAST_EXPECT(env.app().getRuntimeConfig().active());

        // Verify via getConfig
        auto cfg = env.app().getRuntimeConfig().getConfig("*");
        BEAST_EXPECT(cfg.has_value());
        BEAST_EXPECT(cfg->rngClaimDropPctX100 == 5000);

        // Clear and verify removal
        {
            Json::Value params;
            params["clear_all"] = true;
            auto result = runtimeConfig(env, params);
            BEAST_EXPECT(result["configs"].size() == 0);
        }
        BEAST_EXPECT(!env.app().getRuntimeConfig().active());
    }

    void
    testRngClaimDropPctClamping()
    {
        testcase("rng_claim_drop_pct clamped to 0-100");
        using namespace test::jtx;
        Env env{*this};

        // Over 100
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["*"] = Json::objectValue;
            params["set"]["*"]["rng_claim_drop_pct"] = 150.0;
            runtimeConfig(env, params);
        }
        auto cfg = env.app().getRuntimeConfig().getConfig("*");
        BEAST_EXPECT(cfg.has_value());
        BEAST_EXPECT(cfg->rngClaimDropPctX100 == 10000);  // clamped to 100%

        // Negative
        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["*"] = Json::objectValue;
            params["set"]["*"]["rng_claim_drop_pct"] = -10.0;
            runtimeConfig(env, params);
        }
        cfg = env.app().getRuntimeConfig().getConfig("*");
        BEAST_EXPECT(cfg.has_value());
        BEAST_EXPECT(cfg->rngClaimDropPctX100 == 0);  // clamped to 0%
    }

    void
    testRngAndExportRuntimeToggles()
    {
        testcase("rng/export runtime toggles round-trip");
        using namespace test::jtx;
        Env env{*this};

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["*"] = Json::objectValue;
        params["set"]["*"]["bootstrap_fast_start"] = false;
        params["set"]["*"]["rng_poll_ms"] = 5;
        params["set"]["*"]["no_export_sig"] = true;
        auto const result = runtimeConfig(env, params);

        auto const& global = result["configs"]["*"];
        BEAST_EXPECT(global["bootstrap_fast_start"].asBool() == false);
        BEAST_EXPECT(global["rng_poll_ms"].asInt() == 50);
        BEAST_EXPECT(global["no_export_sig"].asBool() == true);

        auto const cfg = env.app().getRuntimeConfig().getConfig("*");
        BEAST_EXPECT(cfg.has_value());
        if (cfg)
        {
            BEAST_EXPECT(cfg->bootstrapFastStart.has_value());
            BEAST_EXPECT(*cfg->bootstrapFastStart == false);
            BEAST_EXPECT(cfg->rngPollMs == 50);
            BEAST_EXPECT(cfg->noExportSig.has_value());
            BEAST_EXPECT(*cfg->noExportSig == true);
        }
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
        testConfigValsMergedDirect();
        testRuntimeConfigDirectEffectiveView();
        testRuntimeConfigDirectInactiveEntries();
        testRuntimeConfigDirectRawAndCategoryHelpers();
        testRuntimeConfigIndividualEnvVars();
        testRuntimeConfigJsonEnvMergesTargets();
        testRuntimeConfigInvalidJsonEnvIgnored();
        testGetEmpty();
        testSetGlobal();
        testSetPerPeer();
        testClear();
        testClearAll();
        testPerPeerWithoutGlobal();
        testMessageTypeFilter();
        testMessageTypeAliases();
        testEnvMessageTypeFilter();
        testInvalidEnvMessageType();
        testMessageTypeFilterEmpty();
        testInvalidMessageType();
        testInvalidMessageTypesShape();
        testDropPctClamping();
        testRngClaimDropPct();
        testRngClaimDropPctClamping();
        testRngAndExportRuntimeToggles();
        testPerPeerClearInheritedFilter();
    }
};

BEAST_DEFINE_TESTSUITE(RuntimeConfig, rpc, ripple);

}  // namespace ripple
