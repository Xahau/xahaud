#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <array>
#include <bit>
#include <limits>
#include <string_view>

namespace hook::quickjs {
namespace {

QuickJSInvocation*
invocationFrom(wasmtime_caller_t* caller) noexcept
{
    auto* context = wasmtime_caller_context(caller);
    return static_cast<QuickJSInvocation*>(wasmtime_context_get_data(context));
}

wasm_trap_t*
callbackTrap(std::string_view message) noexcept
{
    return wasmtime_trap_new(message.data(), message.size());
}

bool
matchesKind(HookHostValueKind expected, wasmtime_valkind_t actual) noexcept
{
    return (isI32(expected) && actual == WASMTIME_I32) ||
        (!isI32(expected) && actual == WASMTIME_I64);
}

wasm_trap_t*
ordinaryUnavailableResult(
    HookHostFunctionDescriptor const& operation,
    wasmtime_val_t* results,
    std::size_t resultCount) noexcept
{
    if (resultCount != 1 || !results)
        return callbackTrap("invalid Xahau Hook callback result shape");
    auto const status =
        static_cast<std::int64_t>(hook_api::hook_return_code::NOT_IMPLEMENTED);
    if (isI32(operation.result))
    {
        results[0] = wasmtime_val_t{
            .kind = WASMTIME_I32,
            .of = {.i32 = static_cast<std::int32_t>(status)}};
        return nullptr;
    }
    if (!isI32(operation.result))
    {
        results[0] =
            wasmtime_val_t{.kind = WASMTIME_I64, .of = {.i64 = status}};
        return nullptr;
    }
    return callbackTrap("unsupported Xahau Hook callback result type");
}

wasm_trap_t*
finishCallback(
    QuickJSInvocation& invocation,
    WasmtimeHostBinding const& binding,
    HookHostCallStatus status,
    HookHostValue const& output,
    wasmtime_val_t* results,
    std::size_t resultCount) noexcept
{
    if (resultCount != 1 || !results)
        return callbackTrap("invalid Xahau Hook callback result shape");

    if (status == HookHostCallStatus::accept ||
        status == HookHostCallStatus::rollback)
    {
        if (binding.terminal != TerminalBehavior::hookTerminal)
            return callbackTrap(
                "ordinary Xahau Hook import returned a terminal code");
        auto const expected = status == HookHostCallStatus::accept
            ? hook_api::ExitType::ACCEPT
            : hook_api::ExitType::ROLLBACK;
        if (invocation.hookCtx.result.exitType != expected)
            return callbackTrap(
                "Xahau Hook terminal lacks matching HookContext state");
        invocation.terminal = true;
        return callbackTrap("Xahau Hook terminal");
    }
    if (status == HookHostCallStatus::trap)
        return callbackTrap("Xahau Hook host operation trapped");

    if (isI32(binding.operation->result))
    {
        if (output.kind != binding.operation->result)
            return callbackTrap("Xahau Hook host result width mismatch");
        results[0] = wasmtime_val_t{
            .kind = WASMTIME_I32,
            .of = {.i32 = std::bit_cast<std::int32_t>(output.asI32())}};
        return nullptr;
    }
    if (!isI32(binding.operation->result))
    {
        if (output.kind != binding.operation->result)
            return callbackTrap("Xahau Hook host result width mismatch");
        results[0] = wasmtime_val_t{
            .kind = WASMTIME_I64,
            .of = {.i64 = std::bit_cast<std::int64_t>(output.asI64())}};
        return nullptr;
    }
    return callbackTrap("unsupported Xahau Hook callback result type");
}

}  // namespace

std::uint64_t
quickJSHostWorkCost(
    QuickJSRuntimeProfile const& profile,
    std::uint64_t declaredBytes) noexcept
{
    return declaredBytes > (std::numeric_limits<std::uint64_t>::max() -
                            profile.hostWorkBasePerCall) /
                profile.hostWorkPerAddressedByte
        ? std::numeric_limits<std::uint64_t>::max()
        : profile.hostWorkBasePerCall +
            declaredBytes * profile.hostWorkPerAddressedByte;
}

QuickJSInvocation::QuickJSInvocation(
    HookContext& hookCtx_,
    beast::Journal const& journal_,
    QuickJSRuntimeProfile const& profile_) noexcept
    : hookCtx(hookCtx_)
    , journal(journal_)
    , profile(profile_)
    , hostWorkRemaining(profile_.hostWorkBudget)
{
}

QuickJSHostCall::QuickJSHostCall(
    QuickJSInvocation& invocation,
    wasmtime_caller_t* caller) noexcept
    : invocation_(invocation), caller_(caller)
{
}

HookContext&
QuickJSHostCall::hookContext() noexcept
{
    return invocation_.hookCtx;
}

beast::Journal const&
QuickJSHostCall::journal() const noexcept
{
    return invocation_.journal;
}

std::optional<HookGuestMemory>
QuickJSHostCall::memory() const noexcept
{
    wasmtime_extern_t memoryExport;
    if (!wasmtime_caller_export_get(caller_, "memory", 6, &memoryExport) ||
        memoryExport.kind != WASMTIME_EXTERN_MEMORY)
        return std::nullopt;
    auto* context = wasmtime_caller_context(caller_);
    return HookGuestMemory{
        wasmtime_memory_data(context, &memoryExport.of.memory),
        wasmtime_memory_data_size(context, &memoryExport.of.memory)};
}

bool
QuickJSHostCall::charge(std::uint64_t declaredBytes) noexcept
{
    auto const& profile = invocation_.profile;
    auto const cost = quickJSHostWorkCost(profile, declaredBytes);
    if (cost > invocation_.hostWorkRemaining)
    {
        fault_ = "Xahau Hook host-work budget exhausted";
        return false;
    }
    invocation_.hostWorkRemaining -= cost;
    return true;
}

char const*
QuickJSHostCall::fault() const noexcept
{
    return fault_;
}

std::uint64_t
declaredHostWork(
    HostWorkMeasureKind measure,
    std::span<wasmtime_val_t const> values) noexcept
{
    switch (measure)
    {
        case HostWorkMeasureKind::zeroV1:
            return 0;
        case HostWorkMeasureKind::argument1V1:
            return values.size() > 1
                ? static_cast<std::uint32_t>(values[1].of.i32)
                : std::numeric_limits<std::uint64_t>::max();
        case HostWorkMeasureKind::arguments1And3SaturatedV1:
            if (values.size() <= 3)
                return std::numeric_limits<std::uint64_t>::max();
            return static_cast<std::uint64_t>(
                       static_cast<std::uint32_t>(values[1].of.i32)) +
                static_cast<std::uint32_t>(values[3].of.i32);
    }
    return std::numeric_limits<std::uint64_t>::max();
}

bool
validWasmtimeHostBinding(WasmtimeHostBinding const& binding) noexcept
{
    if (binding.module.empty() || binding.name.empty() ||
        binding.parameterCount > maxImportParameters || !binding.operation ||
        !binding.operation->function ||
        binding.operation->name != binding.name ||
        binding.operation->implementationVersion !=
            binding.implementationVersion ||
        binding.operation->parameters.size() != binding.parameterCount ||
        binding.operation->result != binding.result)
        return false;
    for (std::size_t parameter = 0;
         parameter < binding.operation->parameters.size();
         ++parameter)
        if (binding.operation->parameters[parameter] !=
            binding.parameters[parameter])
            return false;
    return true;
}

wasm_trap_t*
rawHookCallback(
    void* environment,
    wasmtime_caller_t* caller,
    wasmtime_val_t const* args,
    std::size_t argumentCount,
    wasmtime_val_t* results,
    std::size_t resultCount) noexcept
{
    try
    {
        auto const* binding =
            static_cast<WasmtimeHostBinding const*>(environment);
        if (!binding || !validWasmtimeHostBinding(*binding))
            return callbackTrap("missing Xahau Hook import descriptor");
        auto const& operation = *binding->operation;
        if (argumentCount != operation.parameters.size() ||
            (argumentCount != 0 && !args))
            return callbackTrap("invalid Xahau Hook callback arguments");
        for (std::size_t index = 0; index < argumentCount; ++index)
            if (!matchesKind(operation.parameters[index], args[index].kind))
                return callbackTrap(
                    "invalid Xahau Hook callback argument type");

        auto* invocation = invocationFrom(caller);
        if (!invocation)
            return callbackTrap("missing Xahau Hook invocation context");
        if (binding->amendment != ripple::uint256{} &&
            !invocation->hookCtx.applyCtx.view().rules().enabled(
                binding->amendment))
            return ordinaryUnavailableResult(operation, results, resultCount);
        QuickJSHostCall call{*invocation, caller};
        if (binding->charging == WasmtimeHostBinding::Charging::quickJSV1 &&
            !call.charge(declaredHostWork(
                binding->measure, std::span{args, argumentCount})))
            return callbackTrap(call.fault());

        constexpr std::size_t maxHostParameters = 12;
        if (argumentCount > maxHostParameters)
            return callbackTrap("too many Xahau Hook host arguments");
        std::array<HookHostValue, maxHostParameters> neutralInputs{};
        for (std::size_t index = 0; index < argumentCount; ++index)
        {
            auto const kind = operation.parameters[index];
            if (kind == HookHostValueKind::i32)
                neutralInputs[index] = HookHostValue::i32(
                    static_cast<std::uint32_t>(args[index].of.i32));
            else if (kind == HookHostValueKind::u32)
                neutralInputs[index] = HookHostValue::u32(
                    static_cast<std::uint32_t>(args[index].of.i32));
            else if (kind == HookHostValueKind::i64)
                neutralInputs[index] = HookHostValue::i64(
                    static_cast<std::uint64_t>(args[index].of.i64));
            else
                neutralInputs[index] = HookHostValue::u64(
                    static_cast<std::uint64_t>(args[index].of.i64));
        }

        auto memory = call.memory();
        HookGuestMemory emptyMemory{nullptr, 0};
        auto& guestMemory = memory ? *memory : emptyMemory;
        HookHostValue output{};
        auto const status = operation.function(
            &invocation->hookCtx,
            guestMemory,
            neutralInputs.data(),
            argumentCount,
            &output,
            1);
        if (call.fault())
            return callbackTrap(call.fault());
        return finishCallback(
            *invocation, *binding, status, output, results, resultCount);
    }
    catch (...)
    {
        return callbackTrap("uncaught Xahau Hook host exception");
    }
}

}  // namespace hook::quickjs
