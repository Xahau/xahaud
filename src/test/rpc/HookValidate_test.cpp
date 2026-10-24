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
#include <xrpld/core/Config.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/jss.h>
#include <cctype>
#include <string>

namespace ripple {

// A valid hook exporting both hook() and cbak() (emit test hook).
static char const validHex[] =
    "0061736D0100000001250660027F7F017F60037F7F7E017E60017F017E600001"
    "7E60027F7F017E60047F7F7F7F017E028F010903656E76025F67000003656E76"
    "06616363657074000103656E760C6574786E5F72657365727665000203656E76"
    "0A6C65646765725F736571000303656E760C686F6F6B5F6163636F756E740004"
    "03656E760C6574786E5F64657461696C73000403656E760D6574786E5F666565"
    "5F62617365000403656E7604656D6974000503656E7608726F6C6C6261636B00"
    "01030302020205030100020627067F0141908A040B7F0041820A0B7F00418008"
    "0B7F0041908A040B7F004180080B7F004180080B070F02046362616B00090468"
    "6F6F6B000A0AA883000299800000410141011080808080001A41004100420010"
    "81808080000B88830002037F017E23808080800041206B220124808080800041"
    "0141011080808080001A41011082808080001A4100108380808000A722024105"
    "6A22034118742003410874418080FC07717220034108764180FE037120034118"
    "767272360295888080004100200241016A22034118742003410874418080FC07"
    "717220034108764180FE03712003411876727236028F8880800041C788808000"
    "41141084808080001A41DB88808000418A011085808080001A41004180888080"
    "0041E50110868080800022043C00A188808000410020044208883C00A0888080"
    "00410020044210883C009F88808000410020044218883C009E88808000410020"
    "044220883C009D88808000410020044228883C009C8880800041002004423088"
    "3C009B8880800041002004423888A7413F7141C000723A009A88808000024002"
    "402001412041808880800041E501108780808000427F550D0041E58980800041"
    "0D42D30010888080800021040C010B41F289808000411042D400108180808000"
    "21040B200141206A24808080800020040B0B900202004180080BE50112006322"
    "000000002400000000201A00000000201B000000006840000000000000007321"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0081140000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "000041E5090B1D456D6974206661696C65642E00456D69742073756363656564"
    "65642E00";

class HookValidate_test : public beast::unit_test::suite
{
    static Json::Value
    call(test::jtx::Env& env, std::string const& code)
    {
        Json::Value params(Json::objectValue);
        params[jss::code] = code;
        return env.rpc("json", "hook_validate", to_string(params))[jss::result];
    }

    static std::unique_ptr<Config>
    withRpc(std::unique_ptr<Config> cfg, bool enabled)
    {
        cfg->HOOK_VALIDATE_RPC = enabled;
        return cfg;
    }

    void
    testDisabled()
    {
        testcase("Disabled");
        using namespace test::jtx;
        Env env{*this, envconfig(withRpc, false)};
        auto const r = call(env, validHex);
        BEAST_EXPECT(r[jss::error] == "notSupported");
    }

    void
    testParams()
    {
        testcase("Parameter validation");
        using namespace test::jtx;
        Env env{*this, envconfig(withRpc, true)};

        {
            // missing code
            auto const r = env.rpc("json", "hook_validate", "{}")[jss::result];
            BEAST_EXPECT(r[jss::error] == "invalidParams");
        }
        {
            // code not a string
            auto const r =
                env.rpc("json", "hook_validate", "{\"code\": 1}")[jss::result];
            BEAST_EXPECT(r[jss::error] == "invalidParams");
        }
        {
            // empty, odd-length and non-hex input
            for (auto const& bad : {"", "0", "0061736Z"})
            {
                auto const r = call(env, bad);
                BEAST_EXPECT(r[jss::error] == "invalidParams");
            }
        }
        {
            // one byte over the maximum hook size
            auto const r =
                call(env, std::string(2 * (hook::maxHookWasmSize() + 1), '0'));
            BEAST_EXPECT(r[jss::error] == "invalidParams");
        }
    }

    void
    testValidate()
    {
        testcase("Validate");
        using namespace test::jtx;
        Env env{*this, envconfig(withRpc, true)};

        {
            auto const r = call(env, validHex);
            BEAST_EXPECT(r[jss::valid] == true);
            BEAST_EXPECT(r.isMember(jss::instruction_count_hook));
            BEAST_EXPECT(r.isMember(jss::instruction_count_cbak));
            BEAST_EXPECT(!r.isMember(jss::vm_error));
            BEAST_EXPECT(r[jss::log].isArray());
        }
        {
            // the lowercase hex the toolchains emit is accepted too
            std::string lower = validHex;
            for (auto& c : lower)
                c = std::tolower(static_cast<unsigned char>(c));
            auto const r = call(env, lower);
            BEAST_EXPECT(r[jss::valid] == true);
        }
        {
            // not webassembly
            auto const r = call(env, std::string(140, '0'));
            BEAST_EXPECT(r[jss::valid] == false);
            BEAST_EXPECT(!r.isMember(jss::instruction_count_hook));
            BEAST_EXPECT(r[jss::log].isArray() && r[jss::log].size() > 0);
        }
        {
            std::string bad = validHex;
            bad[2] = 'F';  // corrupt magic header
            auto const r = call(env, bad);
            BEAST_EXPECT(r[jss::valid] == false);
            BEAST_EXPECT(r[jss::log].isArray() && r[jss::log].size() > 0);
        }
        {
            // truncated module
            std::string const truncated = std::string(validHex).substr(0, 200);
            auto const r = call(env, truncated);
            BEAST_EXPECT(r[jss::valid] == false);
        }
    }

    void
    testConfigDefault()
    {
        testcase("Config default");
        auto load = [](bool standalone, char const* content) {
            Config cfg;
            cfg.setupControl(true, true, standalone);
            cfg.loadFromString(content);
            return cfg.HOOK_VALIDATE_RPC;
        };
        BEAST_EXPECT(load(true, ""));
        BEAST_EXPECT(!load(true, "[hook_validate_rpc]\n0\n"));
        BEAST_EXPECT(!load(false, ""));
        BEAST_EXPECT(load(false, "[hook_validate_rpc]\n1\n"));
    }

    void
    run() override
    {
        testConfigDefault();
        testDisabled();
        testParams();
        testValidate();
    }
};

BEAST_DEFINE_TESTSUITE(HookValidate, rpc, ripple);

}  // namespace ripple
