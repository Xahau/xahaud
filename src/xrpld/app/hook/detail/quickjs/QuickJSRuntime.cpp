#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSProviderSession.h>
#include <xrpl/basics/scope.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <wasmtime.h>

namespace hook {

void
executeQuickJSBytecode(
    HookContext& hookCtx,
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode,
    bool callback,
    std::uint32_t reserved,
    beast::Journal const& journal)
{
    auto fail = [&](std::string_view phase, std::string const& detail) {
        JLOG(journal.warn()) << "HookError[" << hookCtx.result.account << '-'
                             << hookCtx.result.otxnAccount << "]: " << phase
                             << (detail.empty() ? "" : ": ") << detail;
        hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
    };

    try
    {
        if (!runtime)
        {
            fail("QuickJS runtime profile is not registered", {});
            return;
        }

        quickjs::ProviderStage stage = quickjs::ProviderStage::none;
        std::string detail;
        quickjs::QuickJSInvocation invocation{
            hookCtx, journal, runtime->profile};
        auto session = quickjs::ProviderSession::create(
            *runtime, &invocation, stage, detail);
        if (!session)
        {
            fail(quickjs::providerStageName(stage), detail);
            return;
        }
        if (!session->initialize(stage, detail) ||
            !session->resetInvocationFuel(stage, detail))
        {
            fail(quickjs::providerStageName(stage), detail);
            return;
        }

        // This guard is created after the session, so it samples fuel before
        // the Store is destroyed on every post-reset exit path.
        ripple::scope_exit recordFuel{[&] {
            if (auto const consumed = session->invocationFuelConsumed())
                hookCtx.result.instructionCount = *consumed;
        }};

        auto const bytecodePointer =
            session->allocateAndCopy(bytecode, stage, detail);
        if (!bytecodePointer)
        {
            fail(quickjs::providerStageName(stage), detail);
            return;
        }

        wasmtime_val_t arguments[3] = {
            {.kind = WASMTIME_I32,
             .of = {.i32 = static_cast<std::int32_t>(*bytecodePointer)}},
            {.kind = WASMTIME_I32,
             .of = {.i32 = static_cast<std::int32_t>(bytecode.size())}},
            {.kind = WASMTIME_I32,
             .of = {.i32 = static_cast<std::int32_t>(reserved)}}};
        wasmtime_val_t result[1];
        if (!session->callExport(
                callback ? "qjs_cbak" : "qjs_hook",
                arguments,
                3,
                result,
                1,
                detail))
        {
            if (invocation.terminal)
                return;
            fail("QuickJS entry invocation trapped", detail);
            return;
        }

        if (result[0].of.i32 != 0)
        {
            detail = session->readDiagnostic();
            hookCtx.result.exitReason = detail;
            fail("QuickJS entry invocation returned an error", detail);
            return;
        }

        fail("JavaScript Hook returned without a terminal", {});
    }
    catch (std::exception const& exception)
    {
        fail("uncaught QuickJS runtime exception", exception.what());
    }
    catch (...)
    {
        fail("uncaught QuickJS runtime exception", {});
    }
}

}  // namespace hook
