#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/hook/HookHostTypes.h>
#include <xrpld/app/hook/HookWasmEngine.h>
#include <xrpl/beast/unit_test/suite.h>
#include "jshooks/raw-catalogue/catalogue_wasm.h"
#include <algorithm>
#include <span>
#include <string>

namespace ripple::test {

class HookWasmtimeCatalogue_test : public beast::unit_test::suite
{
    ApplyContext
    makeApplyContext(jtx::Env& env, OpenView& view, STTx const& transaction)
    {
        return ApplyContext{
            env.app(),
            view,
            transaction,
            tesSUCCESS,
            env.current()->fees().base,
            tapNONE,
            env.journal};
    }

public:
    void
    run() override
    {
        using namespace raw_hook_catalogue_fixture;

        testcase("Independent 75-import Wasm surface");
        hook::HookWasmModuleSurface surface;
        auto const inspectError =
            hook::inspectWasmtimeHookModule(wasm, surface);
        BEAST_EXPECT(!inspectError);
        if (inspectError)
            return;

        auto const& catalogue = hook::hookHostFunctionCatalogue();
        BEAST_EXPECT(surface.imports.size() == catalogue.size());
        BEAST_EXPECT(surface.imports.size() == names.size());
        for (std::size_t index = 0;
             index < catalogue.size() && index < surface.imports.size();
             ++index)
        {
            auto const& actual = surface.imports[index];
            auto const& expected = catalogue[index];
            BEAST_EXPECT(actual.module == "env");
            BEAST_EXPECT(actual.name == expected.name);
            BEAST_EXPECT(actual.name == names[index]);
            BEAST_EXPECT(actual.parameters == expected.parameters);
            BEAST_EXPECT(actual.result == expected.result);
            BEAST_EXPECT(
                std::find(
                    surface.exports.begin(),
                    surface.exports.end(),
                    "call_" + actual.name) != surface.exports.end());
        }
        BEAST_EXPECT(
            std::find(
                surface.exports.begin(), surface.exports.end(), "memory") !=
            surface.exports.end());
        BEAST_EXPECT(
            std::find(surface.exports.begin(), surface.exports.end(), "hook") !=
            surface.exports.end());

        testcase("Every independent wrapper reaches its raw body");
        using namespace jtx;
        Env env{*this, supported_amendments()};
        auto const alice = Account{"alice"};
        env.fund(XRP(1000), alice);
        env.close();

        STTx transaction{ttINVOKE, [&](STObject& object) {
                             object[sfAccount] = alice.id();
                         }};
        OpenView view{*env.current()};
        auto applyContext = makeApplyContext(env, view, transaction);

        for (auto const& name : names)
        {
            hook::HookStateMap state;
            auto context = makeStubHookContext(
                applyContext,
                alice.id(),
                alice.id(),
                {.expected_etxn_count = 1},
                state);
            auto const execution = hook::invokeWasmtimeHookTestExport(
                wasm, "call_" + std::string{name}, context, env.journal);
            BEAST_EXPECTS(
                execution.ok,
                std::string{"Wasmtime did not reach raw body for "} +
                    std::string{name} + ": " +
                    execution.error.value_or("unknown error"));
            if (name == "_g")
            {
                BEAST_EXPECT(!!execution.returnValue);
                if (execution.returnValue)
                {
                    BEAST_EXPECT(
                        execution.returnValue->kind ==
                        hook::HookHostValueKind::i32);
                    BEAST_EXPECT(execution.returnValue->asI32() == 1U);
                }
            }
            if (name == "state")
            {
                BEAST_EXPECT(!!execution.returnValue);
                if (execution.returnValue)
                {
                    BEAST_EXPECT(
                        execution.returnValue->kind ==
                        hook::HookHostValueKind::i64);
                    BEAST_EXPECT(
                        execution.returnValue->asI64() ==
                        static_cast<std::uint64_t>(
                            hook_api::hook_return_code::TOO_SMALL));
                }
            }
        }
    }
};

BEAST_DEFINE_TESTSUITE(HookWasmtimeCatalogue, app, ripple);

}  // namespace ripple::test
