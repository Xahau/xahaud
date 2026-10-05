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
    testRuntimeConfigDirectScopedAccessors()
    {
        testcase("RuntimeConfig direct scoped accessors");

        RuntimeConfig rc;
        rc.clearAllConfigs();
        BEAST_EXPECT(!rc.active());

        ConsensusTestConfig global;
        global.rngPollMs = 100;
        rc.setGlobalConfig(global);

        auto globalCfg = rc.getConsensusTestConfig();
        if (!BEAST_EXPECT(globalCfg.has_value()))
            return;
        BEAST_EXPECT(globalCfg->rngPollMs == 100);
        BEAST_EXPECT(rc.active());

        rc.clearGlobalConfig();
        BEAST_EXPECT(!rc.getConsensusTestConfig().has_value());
        BEAST_EXPECT(!rc.active());
    }

    void
    testRuntimeConfigJsonEnv()
    {
        testcase("XAHAUD_RUNTIME_TEST_CONFIG flat key env");

        EnvVarGuard runtimeJson{
            "XAHAUD_RUNTIME_TEST_CONFIG",
            R"({"set":{"global":{"rng_claim_drop_pct":3.5,)"
            R"("rng_reveal_drop_pct":4.5,"rng_poll_ms":5}}})"};

        RuntimeConfig rc;
        auto global = rc.getConsensusTestConfig();
        if (!BEAST_EXPECT(global.has_value()))
            return;
        BEAST_EXPECT(global->rngClaimDropPctX100 == 350);
        BEAST_EXPECT(global->rngRevealDropPctX100 == 450);
        BEAST_EXPECT(global->rngPollMs == 50);
        BEAST_EXPECT(rc.active());
    }

    void
    testExternalControlsCompiledOut()
    {
        testcase("External runtime_config controls compiled out");

        EnvVarGuard runtimeJson{
            "XAHAUD_RUNTIME_TEST_CONFIG",
            R"({"set":{"global":{"rng_poll_ms":100}}})"};

        RuntimeConfig rc;
        BEAST_EXPECT(!rc.active());
        BEAST_EXPECT(!rc.getConsensusTestConfig().has_value());

        using namespace test::jtx;
        Env env{*this};

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["global"] = Json::objectValue;
        params["set"]["global"]["rng_poll_ms"] = 100;
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
        params["set"]["global"]["rng_claim_drop_pct"] = 10.0;

        auto result = runtimeConfig(env, params);
        BEAST_EXPECT(result.isMember("configs"));

        auto const& configs = result["configs"];
        BEAST_EXPECT(configs.isMember("global"));
        BEAST_EXPECT(configs["global"]["rng_poll_ms"].asInt() == 50);
        BEAST_EXPECT(
            configs["global"]["rng_claim_drop_pct"].asDouble() == 10.0);

        auto global = env.app().getRuntimeConfig().getConsensusTestConfig();
        if (!BEAST_EXPECT(global.has_value()))
            return;
        BEAST_EXPECT(global->rngPollMs == 50);
        BEAST_EXPECT(global->rngClaimDropPctX100 == 1000);
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
            params["set"]["global"] = Json::objectValue;
            params["set"]["global"]["rng_poll_ms"] = 100;
            params["set"]["global"]["rng_claim_drop_pct"] = 10.0;
            runtimeConfig(env, params);
        }

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["global"] = Json::objectValue;
            params["set"]["global"]["rng_reveal_drop_pct"] = 20.0;
            runtimeConfig(env, params);
        }

        auto cfg = env.app().getRuntimeConfig().getConsensusTestConfig();
        if (!BEAST_EXPECT(cfg.has_value()))
            return;
        BEAST_EXPECT(!cfg->rngPollMs.has_value());
        BEAST_EXPECT(!cfg->rngClaimDropPctX100.has_value());
        BEAST_EXPECT(cfg->rngRevealDropPctX100 == 2000);
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
            params["set"]["global"]["rng_poll_ms"] = 100;
            runtimeConfig(env, params);
        }
        BEAST_EXPECT(env.app().getRuntimeConfig().active());

        Json::Value params;
        params["set"] = Json::objectValue;
        params["set"]["global"] = Json::nullValue;
        auto result = runtimeConfig(env, params);

        BEAST_EXPECT(!result["configs"].isMember("global"));
        BEAST_EXPECT(!env.app().getRuntimeConfig().getConsensusTestConfig());
        BEAST_EXPECT(!env.app().getRuntimeConfig().active());
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
        params["set"]["global"]["rng_poll_ms"] = 100;
        runtimeConfig(env, params);

        BEAST_EXPECT(env.app().getRuntimeConfig().active());

        Json::Value clear;
        clear["clear_all"] = true;
        auto result = runtimeConfig(env, clear);
        BEAST_EXPECT(result["configs"].size() == 0);
        BEAST_EXPECT(!env.app().getRuntimeConfig().active());
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
            params["set"]["peer:10.0.0.2:51235"] = Json::objectValue;
            params["set"]["peer:10.0.0.2:51235"]["rng_poll_ms"] = 100;
            expectInvalid(params);
        }

        {
            Json::Value params;
            params["set"] = Json::objectValue;
            params["set"]["peer_defaults"] = Json::objectValue;
            params["set"]["peer_defaults"]["rng_poll_ms"] = 100;
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
            params["set"]["global"] = Json::objectValue;
            params["set"]["global"]["rng_claim_drop_pct"] = 200.0;
            expectInvalid(params);
        }

        {
            Json::Value params;
            params["set"] = Json::arrayValue;
            expectInvalid(params);
        }
    }

public:
    void
    run() override
    {
        testRuntimeConfigDirectScopedAccessors();
#ifdef XAHAUD_ENABLE_RUNTIME_TEST_CONFIG
        testRuntimeConfigJsonEnv();
        testInvalidJsonEnvIgnored();
        testGetEmpty();
        testSetFlatKeys();
        testSetReplacesWholeObject();
        testNullClearsKeys();
        testClearAll();
        testInvalidInputs();
#else
        testExternalControlsCompiledOut();
#endif
    }
};

BEAST_DEFINE_TESTSUITE(RuntimeConfig, rpc, ripple);

}  // namespace ripple
