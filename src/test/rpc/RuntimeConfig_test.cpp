//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
    SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
    IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
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

    Json::Value
    runtimeConfig(test::jtx::Env& env, Json::Value const& params)
    {
        return env.rpc(
            "json", "runtime_config", to_string(params))[jss::result];
    }

    Json::Value
    runtimeConfig(test::jtx::Env& env)
    {
        return env.rpc("runtime_config")[jss::result];
    }

    void
    testPeerFaultConfigMergeAndCategories()
    {
        testcase("PeerFaultConfig merge and categories");

        PeerFaultConfig defaults;
        defaults.sendDelayMs = 100;
        defaults.sendDropPctX100 = 1000;
        defaults.messageCategories =
            std::set<std::size_t>{TrafficCount::category::proposal};

        PeerFaultConfig peer;
        peer.sendDelayJitterMs = 25;
        peer.messageCategories = std::set<std::size_t>{};

        auto const merged = defaults.merged(peer);
        BEAST_EXPECT(merged.sendDelayMs == 100);
        BEAST_EXPECT(merged.sendDelayJitterMs == 25);
        BEAST_EXPECT(merged.sendDropPctX100 == 1000);
        BEAST_EXPECT(merged.messageCategories.has_value());
        BEAST_EXPECT(merged.messageCategories->empty());
        BEAST_EXPECT(merged.appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(merged.appliesTo(TrafficCount::category::validation));

        PeerFaultConfig inactive;
        inactive.sendDelayMs = 0;
        inactive.sendDelayJitterMs = 0;
        inactive.sendDropPctX100 = 0;
        inactive.messageCategories =
            std::set<std::size_t>{TrafficCount::category::proposal};
        BEAST_EXPECT(!inactive.active());
    }

    void
    testRuntimeConfigDirectScopedAccessors()
    {
        testcase("RuntimeConfig direct scoped accessors");

        RuntimeConfig rc;
        rc.clearAllConfigs();

        ConsensusTestConfig global;
        global.bootstrapFastStart = true;
        rc.setGlobalConfig(global);

        auto globalCfg = rc.getConsensusTestConfig();
        if (!BEAST_EXPECT(globalCfg.has_value()))
            return;
        BEAST_EXPECT(globalCfg->bootstrapFastStart.has_value());
        BEAST_EXPECT(*globalCfg->bootstrapFastStart == true);

        PeerFaultConfig defaults;
        defaults.sendDelayMs = 100;
        defaults.sendDropPctX100 = 1000;
        rc.setPeerDefaults(defaults);

        PeerFaultConfig peer;
        peer.sendDelayMs = 500;
        rc.setPeerConfig("10.0.0.2:51235", peer);

        auto peerCfg = rc.getPeerFaultConfig("10.0.0.2:51235");
        if (!BEAST_EXPECT(peerCfg.has_value()))
            return;
        BEAST_EXPECT(peerCfg->sendDelayMs == 500);
        BEAST_EXPECT(peerCfg->sendDropPctX100 == 1000);
        BEAST_EXPECT(rc.active());

        auto otherCfg = rc.getPeerFaultConfig("10.0.0.3:51235");
        if (!BEAST_EXPECT(otherCfg.has_value()))
            return;
        BEAST_EXPECT(otherCfg->sendDelayMs == 100);
        BEAST_EXPECT(otherCfg->sendDropPctX100 == 1000);

        rc.clearPeerDefaults();
        peerCfg = rc.getPeerFaultConfig("10.0.0.2:51235");
        if (!BEAST_EXPECT(peerCfg.has_value()))
            return;
        BEAST_EXPECT(peerCfg->sendDelayMs == 500);
        BEAST_EXPECT(!peerCfg->sendDropPctX100.has_value());
        BEAST_EXPECT(!rc.getPeerFaultConfig("10.0.0.3:51235").has_value());

        rc.clearPeerConfig("10.0.0.2:51235");
        rc.clearGlobalConfig();
        BEAST_EXPECT(!rc.active());
    }

    void
    testRuntimeConfigJsonEnv()
    {
        testcase("XAHAUD_RUNTIME_TEST_CONFIG flat key env");

        EnvVarGuard runtimeJson{
            "XAHAUD_RUNTIME_TEST_CONFIG",
            R"({"set":{"global":{"rng_claim_drop_pct":3.5,)"
            R"("rng_reveal_drop_pct":4.5,)"
            R"("bootstrap_fast_start":false,"rng_poll_ms":5,)"
            R"("no_export_sig":true},)"
            R"("peer_defaults":{"send_delay_ms":100,)"
            R"("send_delay_jitter_ms":20,"send_drop_pct":1.25,)"
            R"("message_types":["proposal"]},)"
            R"("peer:10.0.0.5:51235":{"send_delay_ms":200,)"
            R"("send_drop_pct":2.5,"message_types":[]}}})"};

        RuntimeConfig rc;
        auto global = rc.getConsensusTestConfig();
        if (!BEAST_EXPECT(global.has_value()))
            return;
        BEAST_EXPECT(global->rngClaimDropPctX100 == 350);
        BEAST_EXPECT(global->rngRevealDropPctX100 == 450);
        BEAST_EXPECT(global->bootstrapFastStart.has_value());
        BEAST_EXPECT(*global->bootstrapFastStart == false);
        BEAST_EXPECT(global->rngPollMs == 50);
        BEAST_EXPECT(global->noExportSig.has_value());
        BEAST_EXPECT(*global->noExportSig == true);

        auto defaults = rc.getPeerFaultConfig("10.0.0.6:51235");
        if (!BEAST_EXPECT(defaults.has_value()))
            return;
        BEAST_EXPECT(defaults->sendDelayMs == 100);
        BEAST_EXPECT(defaults->sendDelayJitterMs == 20);
        BEAST_EXPECT(defaults->sendDropPctX100 == 125);
        BEAST_EXPECT(defaults->appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(!defaults->appliesTo(TrafficCount::category::validation));

        auto peer = rc.getPeerFaultConfig("10.0.0.5:51235");
        if (!BEAST_EXPECT(peer.has_value()))
            return;
        BEAST_EXPECT(peer->sendDelayMs == 200);
        BEAST_EXPECT(peer->sendDelayJitterMs == 20);
        BEAST_EXPECT(peer->sendDropPctX100 == 250);
        BEAST_EXPECT(peer->appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(peer->appliesTo(TrafficCount::category::validation));
    }

    void
    testExternalControlsCompiledOut()
    {
        testcase("External runtime_config controls compiled out");

        EnvVarGuard runtimeJson{
            "XAHAUD_RUNTIME_TEST_CONFIG",
            R"({"set":{"global":{"no_export_sig":true}}})"};

        RuntimeConfig rc;
        BEAST_EXPECT(!rc.active());
        BEAST_EXPECT(!rc.getConsensusTestConfig().has_value());

        using namespace test::jtx;
        Env env{*this};

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["global"] = Json::objectValue;
        params["set"]["global"]["no_export_sig"] = true;
        auto result = runtimeConfig(env, params);
        BEAST_EXPECT(result.isMember("error"));
        BEAST_EXPECT(result["error"].asString() == "invalidParams");
        BEAST_EXPECT(!env.app().getRuntimeConfig().active());
    }

    void
    testInvalidJsonEnvIgnored()
    {
        testcase("Invalid XAHAUD_RUNTIME_TEST_CONFIG JSON is ignored");

        EnvVarGuard runtimeJson{"XAHAUD_RUNTIME_TEST_CONFIG", R"([])"};

        RuntimeConfig rc;
        BEAST_EXPECT(!rc.active());
        BEAST_EXPECT(!rc.getConsensusTestConfig().has_value());
        BEAST_EXPECT(!rc.getPeerFaultConfig("10.0.0.1:51235").has_value());
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
    testSetFlatKeys()
    {
        testcase("SET flat keys");
        using namespace test::jtx;
        Env env{*this};

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["global"] = Json::objectValue;
        params["set"]["global"]["rng_poll_ms"] = 5;
        params["set"]["global"]["no_export_sig"] = true;
        params["set"]["peer_defaults"] = Json::objectValue;
        params["set"]["peer_defaults"]["send_delay_ms"] = 100;
        params["set"]["peer_defaults"]["send_drop_pct"] = 10.0;
        params["set"]["peer:10.0.0.2:51235"] = Json::objectValue;
        params["set"]["peer:10.0.0.2:51235"]["send_delay_ms"] = 500;

        auto result = runtimeConfig(env, params);
        BEAST_EXPECT(result.isMember("configs"));

        auto const& configs = result["configs"];
        BEAST_EXPECT(configs.isMember("global"));
        BEAST_EXPECT(configs["global"]["rng_poll_ms"].asInt() == 50);
        BEAST_EXPECT(configs["global"]["no_export_sig"].asBool() == true);
        BEAST_EXPECT(configs.isMember("peer_defaults"));
        BEAST_EXPECT(configs.isMember("peer:10.0.0.2:51235"));

        auto& rc = env.app().getRuntimeConfig();
        auto global = rc.getConsensusTestConfig();
        BEAST_EXPECT(global.has_value());
        BEAST_EXPECT(global->rngPollMs == 50);
        BEAST_EXPECT(global->noExportSig.has_value());
        BEAST_EXPECT(*global->noExportSig == true);

        auto peerCfg = rc.getPeerFaultConfig("10.0.0.2:51235");
        if (!BEAST_EXPECT(peerCfg.has_value()))
            return;
        BEAST_EXPECT(peerCfg->sendDelayMs == 500);
        BEAST_EXPECT(peerCfg->sendDropPctX100 == 1000);
    }

    void
    testSetReplacesWholeObject()
    {
        testcase("SET replaces whole object");
        using namespace test::jtx;
        Env env{*this};

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["peer_defaults"] = Json::objectValue;
            params["set"]["peer_defaults"]["send_delay_ms"] = 100;
            params["set"]["peer_defaults"]["send_drop_pct"] = 10.0;
            runtimeConfig(env, params);
        }

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["peer_defaults"] = Json::objectValue;
            params["set"]["peer_defaults"]["send_delay_jitter_ms"] = 20;
            runtimeConfig(env, params);
        }

        auto cfg =
            env.app().getRuntimeConfig().getPeerFaultConfig("10.0.0.1:51235");
        if (!BEAST_EXPECT(cfg.has_value()))
            return;
        BEAST_EXPECT(!cfg->sendDelayMs.has_value());
        BEAST_EXPECT(cfg->sendDelayJitterMs == 20);
        BEAST_EXPECT(!cfg->sendDropPctX100.has_value());
    }

    void
    testNullClearsKeys()
    {
        testcase("SET null clears keys");
        using namespace test::jtx;
        Env env{*this};

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["global"] = Json::objectValue;
            params["set"]["global"]["no_export_sig"] = true;
            params["set"]["peer_defaults"] = Json::objectValue;
            params["set"]["peer_defaults"]["send_delay_ms"] = 50;
            params["set"]["peer:10.0.0.2:51235"] = Json::objectValue;
            params["set"]["peer:10.0.0.2:51235"]["send_delay_ms"] = 200;
            runtimeConfig(env, params);
        }

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["global"] = Json::nullValue;
        params["set"]["peer:10.0.0.2:51235"] = Json::nullValue;
        auto result = runtimeConfig(env, params);

        BEAST_EXPECT(!result["configs"].isMember("global"));
        BEAST_EXPECT(result["configs"].isMember("peer_defaults"));
        BEAST_EXPECT(!result["configs"].isMember("peer:10.0.0.2:51235"));

        auto cfg =
            env.app().getRuntimeConfig().getPeerFaultConfig("10.0.0.2:51235");
        BEAST_EXPECT(cfg.has_value());
        BEAST_EXPECT(cfg->sendDelayMs == 50);
        BEAST_EXPECT(!env.app().getRuntimeConfig().getConsensusTestConfig());
    }

    void
    testClearAll()
    {
        testcase("CLEAR_ALL");
        using namespace test::jtx;
        Env env{*this};

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["global"] = Json::objectValue;
        params["set"]["global"]["no_export_sig"] = true;
        params["set"]["peer_defaults"] = Json::objectValue;
        params["set"]["peer_defaults"]["send_drop_pct"] = 50.0;
        runtimeConfig(env, params);

        BEAST_EXPECT(env.app().getRuntimeConfig().active());

        Json::Value clear;
        clear["clear_all"] = true;
        auto result = runtimeConfig(env, clear);
        BEAST_EXPECT(result["configs"].size() == 0);
        BEAST_EXPECT(!env.app().getRuntimeConfig().active());
    }

    void
    testMessageTypeAliases()
    {
        testcase("Message type aliases expand to multiple categories");
        using namespace test::jtx;
        Env env{*this};

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["peer_defaults"] = Json::objectValue;
        params["set"]["peer_defaults"]["send_delay_ms"] = 100;
        params["set"]["peer_defaults"]["message_types"] = Json::arrayValue;
        params["set"]["peer_defaults"]["message_types"].append(
            "candidate_set_fetch");
        auto result = runtimeConfig(env, params);

        auto const& defaults = result["configs"]["peer_defaults"];
        BEAST_EXPECT(defaults.isMember("message_types"));
        BEAST_EXPECT(defaults["message_types"].size() == 4);

        auto cfg =
            env.app().getRuntimeConfig().getPeerFaultConfig("10.0.0.1:51235");
        if (!BEAST_EXPECT(cfg.has_value()))
            return;

        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::gl_tsc_get));
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::gl_tsc_share));
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::ld_tsc_get));
        BEAST_EXPECT(cfg->appliesTo(TrafficCount::category::ld_tsc_share));
        BEAST_EXPECT(!cfg->appliesTo(TrafficCount::category::proposal));
    }

    void
    testInvalidInputs()
    {
        testcase("Invalid runtime_config inputs");
        using namespace test::jtx;
        Env env{*this};

        auto expectInvalid = [&](Json::Value const& params) {
            auto result = runtimeConfig(env, params);
            BEAST_EXPECT(result.isMember("error"));
            BEAST_EXPECT(result["error"].asString() == "invalidParams");
            BEAST_EXPECT(!env.app().getRuntimeConfig().active());
        };

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["global"] = Json::objectValue;
            params["set"]["global"]["send_drop_pct"] = 100.0;
            expectInvalid(params);
        }

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["10.0.0.2:51235"] = Json::objectValue;
            params["set"]["10.0.0.2:51235"]["send_delay_ms"] = 100;
            expectInvalid(params);
        }

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["peer_defaults"] = Json::objectValue;
            params["set"]["peer_defaults"]["message_types"] = "proposal";
            expectInvalid(params);
        }

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["global"] = Json::objectValue;
            params["set"]["global"]["bootstrap_fast_start"] = "true";
            expectInvalid(params);
        }

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["global"] = Json::objectValue;
            params["set"]["global"]["rng_poll_ms"] = "333";
            expectInvalid(params);
        }

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["peer_defaults"] = Json::objectValue;
            params["set"]["peer_defaults"]["message_types"] = Json::arrayValue;
            params["set"]["peer_defaults"]["message_types"].append("proposals");
            expectInvalid(params);
        }

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["peer_defaults"] = Json::objectValue;
            params["set"]["peer_defaults"]["message_types"] = Json::arrayValue;
            params["set"]["peer_defaults"]["message_types"].append(5);
            expectInvalid(params);
        }

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["peer_defaults"] = Json::objectValue;
            params["set"]["peer_defaults"]["send_drop_pct"] = 200.0;
            expectInvalid(params);
        }
    }

    void
    testPerPeerClearsInheritedFilter()
    {
        testcase("Per-peer can override default filter to all");
        using namespace test::jtx;
        Env env{*this};

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["peer_defaults"] = Json::objectValue;
            params["set"]["peer_defaults"]["send_delay_ms"] = 100;
            params["set"]["peer_defaults"]["message_types"] = Json::arrayValue;
            params["set"]["peer_defaults"]["message_types"].append("proposal");
            runtimeConfig(env, params);
        }

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["peer:10.0.0.2:51235"] = Json::objectValue;
            params["set"]["peer:10.0.0.2:51235"]["message_types"] =
                Json::arrayValue;
            runtimeConfig(env, params);
        }

        auto& rc = env.app().getRuntimeConfig();
        auto peerCfg = rc.getPeerFaultConfig("10.0.0.2:51235");
        BEAST_EXPECT(peerCfg.has_value());
        BEAST_EXPECT(peerCfg->appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(peerCfg->appliesTo(TrafficCount::category::validation));
        BEAST_EXPECT(peerCfg->appliesTo(TrafficCount::category::transaction));

        auto otherCfg = rc.getPeerFaultConfig("10.0.0.3:51235");
        BEAST_EXPECT(otherCfg.has_value());
        BEAST_EXPECT(otherCfg->appliesTo(TrafficCount::category::proposal));
        BEAST_EXPECT(!otherCfg->appliesTo(TrafficCount::category::validation));
    }

public:
    void
    run() override
    {
        testPeerFaultConfigMergeAndCategories();
        testRuntimeConfigDirectScopedAccessors();
#ifdef XAHAUD_ENABLE_RUNTIME_TEST_CONFIG
        testRuntimeConfigJsonEnv();
        testInvalidJsonEnvIgnored();
        testGetEmpty();
        testSetFlatKeys();
        testSetReplacesWholeObject();
        testNullClearsKeys();
        testClearAll();
        testMessageTypeAliases();
        testInvalidInputs();
        testPerPeerClearsInheritedFilter();
#else
        testExternalControlsCompiledOut();
#endif
    }
};

BEAST_DEFINE_TESTSUITE(RuntimeConfig, rpc, ripple);

}  // namespace ripple
