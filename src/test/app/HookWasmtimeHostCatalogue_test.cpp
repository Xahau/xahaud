#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSLinker.h>
#include <xrpl/beast/unit_test/suite.h>
#include "jshooks/raw-catalogue/catalogue_wasm.h"
#include <algorithm>
#include <bit>
#include <memory>
#include <string>
#include <vector>

namespace ripple::test {
namespace {

using EnginePtr = std::unique_ptr<wasm_engine_t, decltype(&wasm_engine_delete)>;
using LinkerPtr =
    std::unique_ptr<wasmtime_linker_t, decltype(&wasmtime_linker_delete)>;
using ModulePtr =
    std::unique_ptr<wasmtime_module_t, decltype(&wasmtime_module_delete)>;
using StorePtr =
    std::unique_ptr<wasmtime_store_t, decltype(&wasmtime_store_delete)>;

std::string
takeError(wasmtime_error_t* error)
{
    wasm_name_t message;
    wasmtime_error_message(error, &message);
    std::string result{message.data, message.size};
    wasm_byte_vec_delete(&message);
    wasmtime_error_delete(error);
    return result;
}

std::string
takeTrap(wasm_trap_t* trap)
{
    wasm_name_t message;
    wasm_trap_message(trap, &message);
    std::string result{message.data, message.size};
    wasm_byte_vec_delete(&message);
    wasm_trap_delete(trap);
    return result;
}

EnginePtr
makeEngine()
{
    auto* configuration = wasm_config_new();
    if (!configuration)
        return {nullptr, &wasm_engine_delete};
    wasmtime_config_cranelift_nan_canonicalization_set(configuration, true);
    wasmtime_config_wasm_threads_set(configuration, false);
    wasmtime_config_wasm_relaxed_simd_set(configuration, false);
    wasmtime_config_wasm_memory64_set(configuration, false);
    wasmtime_config_wasm_multi_memory_set(configuration, false);
    wasmtime_config_wasm_tail_call_set(configuration, false);
    return {wasm_engine_new_with_config(configuration), &wasm_engine_delete};
}

std::vector<hook::quickjs::WasmtimeHostBinding>
allEnabledBindings()
{
    using namespace hook;
    using namespace hook::quickjs;
    std::vector<WasmtimeHostBinding> result;
    result.reserve(hookHostFunctionCatalogue().size());
    for (auto const& operation : hookHostFunctionCatalogue())
    {
        std::array<HookHostValueKind, maxImportParameters> parameters{};
        std::copy(
            operation.parameters.begin(),
            operation.parameters.end(),
            parameters.begin());
        result.push_back(
            {.module = "env",
             .name = operation.name,
             .parameters = parameters,
             .parameterCount =
                 static_cast<std::uint8_t>(operation.parameters.size()),
             .result = operation.result,
             .implementationVersion = operation.implementationVersion,
             .operation = &operation,
             .amendment = {},
             .measure = HostWorkMeasureKind::zeroV1,
             .terminal = operation.name == "accept" ||
                     operation.name == "rollback" || operation.name == "_g"
                 ? TerminalBehavior::hookTerminal
                 : TerminalBehavior::ordinaryStatus,
             .charging = WasmtimeHostBinding::Charging::none});
    }
    return result;
}

}  // namespace

class HookWasmtimeHostCatalogue_test : public beast::unit_test::suite
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
        using namespace hook;
        using namespace hook::quickjs;
        using namespace jtx;
        using namespace raw_hook_catalogue_fixture;

        testcase("All 75 independent imports bind one generic callback");
        auto engine = makeEngine();
        BEAST_EXPECT(!!engine);
        if (!engine)
            return;

        wasmtime_module_t* rawModule = nullptr;
        auto* compileError = wasmtime_module_new(
            engine.get(), wasm.data(), wasm.size(), &rawModule);
        if (compileError)
        {
            BEAST_EXPECTS(false, takeError(compileError));
            return;
        }
        BEAST_EXPECT(true);
        ModulePtr module{rawModule, &wasmtime_module_delete};

        auto bindings = allEnabledBindings();
        BEAST_EXPECT(bindings.size() == 75);
        QuickJSHostAdapterPolicy const testPolicy{
            .id = "all-host-functions-test-only",
            .hostWorkMeter = "none",
            .chargeOrder = HostChargeOrder::amendmentBeforeCharge,
            .debitBehavior = HostDebitBehavior::saturatedBasePlusAddressedBytes,
            .imports = bindings};
        BEAST_EXPECT(testPolicy.complete());

        LinkerPtr linker{
            wasmtime_linker_new(engine.get()), &wasmtime_linker_delete};
        BEAST_EXPECT(!!linker);
        if (!linker)
            return;
        std::string linkError;
        auto const linked = defineImports(linker.get(), testPolicy, linkError);
        BEAST_EXPECTS(linked, linkError);
        if (!linked)
            return;

        Env env{*this, supported_amendments()};
        auto const alice = Account{"alice"};
        env.fund(XRP(1000), alice);
        env.close();
        STTx transaction{ttINVOKE, [&](STObject& object) {
                             object[sfAccount] = alice.id();
                         }};
        OpenView view{*env.current()};
        auto applyContext = makeApplyContext(env, view, transaction);
        auto const profile = currentQuickJSRuntimeProfile();

        BEAST_EXPECT(names.size() == bindings.size());
        for (auto const name : names)
        {
            HookStateMap state;
            auto hookContext = makeStubHookContext(
                applyContext,
                alice.id(),
                alice.id(),
                {.expected_etxn_count = 1},
                state);
            QuickJSInvocation invocation{hookContext, env.journal, profile};
            StorePtr store{
                wasmtime_store_new(engine.get(), &invocation, nullptr),
                &wasmtime_store_delete};
            BEAST_EXPECTS(!!store, std::string{name});
            if (!store)
                continue;
            auto* context = wasmtime_store_context(store.get());

            wasmtime_instance_t instance;
            wasm_trap_t* instantiateTrap = nullptr;
            auto* instantiateError = wasmtime_linker_instantiate(
                linker.get(),
                context,
                module.get(),
                &instance,
                &instantiateTrap);
            auto const instantiateFailed =
                instantiateError != nullptr || instantiateTrap != nullptr;
            auto const instantiateDetail = instantiateError
                ? takeError(instantiateError)
                : instantiateTrap ? takeTrap(instantiateTrap)
                                  : std::string{};
            BEAST_EXPECTS(
                !instantiateFailed,
                std::string{name} + ": " + instantiateDetail);
            if (instantiateFailed)
                continue;

            auto const exportName = "call_" + std::string{name};
            wasmtime_extern_t entry;
            auto const found = wasmtime_instance_export_get(
                context,
                &instance,
                exportName.data(),
                exportName.size(),
                &entry);
            BEAST_EXPECTS(
                found && entry.kind == WASMTIME_EXTERN_FUNC, exportName);
            if (!found || entry.kind != WASMTIME_EXTERN_FUNC)
                continue;

            wasmtime_val_t result{};
            wasm_trap_t* callTrap = nullptr;
            auto* callError = wasmtime_func_call(
                context, &entry.of.func, nullptr, 0, &result, 1, &callTrap);
            auto const terminal = name == "accept" || name == "rollback";
            auto const hadCallError = callError != nullptr;
            auto const hadCallTrap = callTrap != nullptr;
            auto const callDetail = callError ? takeError(callError)
                : callTrap                    ? takeTrap(callTrap)
                                              : std::string{};
            BEAST_EXPECTS(
                terminal ? invocation.terminal && (hadCallError || hadCallTrap)
                         : !hadCallError && !hadCallTrap,
                std::string{name} + ": " + callDetail);
            if (terminal)
                BEAST_EXPECT(
                    hookContext.result.exitType ==
                    (name == "accept" ? hook_api::ExitType::ACCEPT
                                      : hook_api::ExitType::ROLLBACK));

            if (!terminal && !hadCallError && !hadCallTrap && name == "_g")
            {
                BEAST_EXPECT(result.kind == WASMTIME_I32);
                BEAST_EXPECT(result.of.i32 == 1);
            }
            if (!terminal && !hadCallError && !hadCallTrap && name == "state")
            {
                BEAST_EXPECT(result.kind == WASMTIME_I64);
                BEAST_EXPECT(
                    result.of.i64 ==
                    static_cast<std::int64_t>(
                        hook_api::hook_return_code::TOO_SMALL));
            }

            if (name == "_g" && !hadCallError && !hadCallTrap)
            {
                constexpr std::string_view guardViolation = "call__g_rollback";
                wasmtime_extern_t violationEntry;
                auto const violationFound = wasmtime_instance_export_get(
                    context,
                    &instance,
                    guardViolation.data(),
                    guardViolation.size(),
                    &violationEntry);
                BEAST_EXPECT(
                    violationFound &&
                    violationEntry.kind == WASMTIME_EXTERN_FUNC);
                if (!violationFound ||
                    violationEntry.kind != WASMTIME_EXTERN_FUNC)
                    continue;

                wasmtime_val_t violationResult{};
                wasm_trap_t* violationTrap = nullptr;
                auto* violationError = wasmtime_func_call(
                    context,
                    &violationEntry.of.func,
                    nullptr,
                    0,
                    &violationResult,
                    1,
                    &violationTrap);
                auto const hadViolationError = violationError != nullptr;
                auto const hadViolationTrap = violationTrap != nullptr;
                auto const violationDetail = violationError
                    ? takeError(violationError)
                    : violationTrap ? takeTrap(violationTrap)
                                    : std::string{};
                BEAST_EXPECTS(
                    invocation.terminal &&
                        (hadViolationError || hadViolationTrap),
                    violationDetail);
                BEAST_EXPECT(
                    hookContext.result.exitType ==
                    hook_api::ExitType::ROLLBACK);
                BEAST_EXPECT(
                    hookContext.result.exitCode ==
                    static_cast<std::int64_t>(
                        hook_api::hook_return_code::GUARD_VIOLATION));
            }
        }
    }
};

BEAST_DEFINE_TESTSUITE(HookWasmtimeHostCatalogue, app, ripple);

}  // namespace ripple::test
