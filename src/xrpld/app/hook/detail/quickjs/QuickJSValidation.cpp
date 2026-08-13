#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSProviderSession.h>
#include <xrpl/basics/scope.h>
#include <cstdint>
#include <limits>
#include <string>
#include <wasmtime.h>

namespace hook {
namespace {

std::optional<std::string>
validate(
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode,
    bool& hasCallback,
    std::uint64_t* invocationFuelConsumed)
{
    hasCallback = false;
    if (!runtime)
        return "QuickJS runtime profile is not registered";
    if (bytecode.empty() ||
        bytecode.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        return "QuickJS Hook bytecode is empty or too large";

    quickjs::ProviderStage stage = quickjs::ProviderStage::none;
    std::string detail;
    auto const failure = [&]() {
        return detail.empty() ? std::string{quickjs::providerStageName(stage)}
                              : detail;
    };
    auto session =
        quickjs::ProviderSession::create(*runtime, nullptr, stage, detail);
    if (!session)
        return failure();
    if (!session->initialize(stage, detail) ||
        !session->resetInvocationFuel(stage, detail))
        return failure();

    ripple::scope_exit recordFuel{[&] {
        if (invocationFuelConsumed)
            if (auto const consumed = session->invocationFuelConsumed())
                *invocationFuelConsumed = *consumed;
    }};

    auto const pointer = session->allocateAndCopy(bytecode, stage, detail);
    if (!pointer)
        return failure();

    wasmtime_val_t arguments[2] = {
        {.kind = WASMTIME_I32,
         .of = {.i32 = static_cast<std::int32_t>(*pointer)}},
        {.kind = WASMTIME_I32,
         .of = {.i32 = static_cast<std::int32_t>(bytecode.size())}}};
    wasmtime_val_t result[1];
    if (!session->callExport(
            "qjs_validate_hook_module", arguments, 2, result, 1, detail))
        return detail;

    auto const flags = result[0].of.i32;
    if (flags != 1 && flags != 3)
    {
        detail = session->readDiagnostic();
        return detail.empty() ? "QuickJS Hook bytecode validation failed"
                              : detail;
    }
    hasCallback = (flags & 2) != 0;
    return std::nullopt;
}

}  // namespace

std::optional<std::string>
validateQuickJSBytecode(
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode,
    bool& hasCallback)
{
    return validate(runtime, bytecode, hasCallback, nullptr);
}

#ifdef ENABLE_TESTS
QuickJSValidationForTests
validateQuickJSBytecodeForTests(
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode)
{
    QuickJSValidationForTests result;
    result.error = validate(
        runtime, bytecode, result.hasCallback, &result.invocationFuelConsumed);
    return result;
}
#endif

}  // namespace hook
