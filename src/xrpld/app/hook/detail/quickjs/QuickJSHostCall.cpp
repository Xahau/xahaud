#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
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
matchesKind(wasm_valkind_t expected, wasmtime_valkind_t actual) noexcept
{
    return (expected == WASM_I32 && actual == WASMTIME_I32) ||
        (expected == WASM_I64 && actual == WASMTIME_I64);
}

wasm_trap_t*
ordinaryUnavailableResult(
    QuickJSV1ImportDescriptor const& descriptor,
    wasmtime_val_t* results,
    std::size_t resultCount) noexcept
{
    if (resultCount != 1 || !results)
        return callbackTrap("invalid Xahau Hook callback result shape");
    auto const status =
        static_cast<std::int64_t>(hook_api::hook_return_code::NOT_IMPLEMENTED);
    if (descriptor.resultKind == WASM_I32)
    {
        results[0] = wasmtime_val_t{
            .kind = WASMTIME_I32,
            .of = {.i32 = static_cast<std::int32_t>(status)}};
        return nullptr;
    }
    if (descriptor.resultKind == WASM_I64)
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
    QuickJSV1ImportDescriptor const& descriptor,
    raw::Result const& result,
    wasmtime_val_t* results,
    std::size_t resultCount) noexcept
{
    if (resultCount != 1 || !results)
        return callbackTrap("invalid Xahau Hook callback result shape");

    if (auto const* code = std::get_if<hook_api::hook_return_code>(&result))
    {
        if (*code == hook_api::hook_return_code::RC_ACCEPT ||
            *code == hook_api::hook_return_code::RC_ROLLBACK)
        {
            if (descriptor.terminal != TerminalBehavior::hookTerminal)
                return callbackTrap(
                    "ordinary Xahau Hook import returned a terminal code");
            auto const expected = *code == hook_api::hook_return_code::RC_ACCEPT
                ? hook_api::ExitType::ACCEPT
                : hook_api::ExitType::ROLLBACK;
            if (invocation.hookCtx.result.exitType != expected)
                return callbackTrap(
                    "Xahau Hook terminal lacks matching HookContext state");
            invocation.terminal = true;
            return callbackTrap("Xahau Hook terminal");
        }
        auto const status = static_cast<std::int64_t>(*code);
        if (descriptor.resultKind == WASM_I32)
        {
            results[0] = wasmtime_val_t{
                .kind = WASMTIME_I32,
                .of = {.i32 = static_cast<std::int32_t>(status)}};
            return nullptr;
        }
        results[0] =
            wasmtime_val_t{.kind = WASMTIME_I64, .of = {.i64 = status}};
        return nullptr;
    }

    auto const value = std::get<std::uint64_t>(result);
    if (descriptor.resultKind == WASM_I32)
    {
        if (value > std::numeric_limits<std::uint32_t>::max())
            return callbackTrap("Xahau Hook i32 result overflow");
        results[0] = wasmtime_val_t{
            .kind = WASMTIME_I32,
            .of = {
                .i32 = std::bit_cast<std::int32_t>(
                    static_cast<std::uint32_t>(value))}};
        return nullptr;
    }
    if (descriptor.resultKind == WASM_I64)
    {
        results[0] = wasmtime_val_t{
            .kind = WASMTIME_I64,
            .of = {.i64 = std::bit_cast<std::int64_t>(value)}};
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
        auto const* resolved =
            static_cast<ResolvedJSImport const*>(environment);
        if (!resolved || !resolved->descriptor)
            return callbackTrap("missing Xahau Hook import descriptor");
        auto const& descriptor = *resolved->descriptor;
        if (argumentCount != descriptor.parameterCount ||
            (argumentCount != 0 && !args))
            return callbackTrap("invalid Xahau Hook callback arguments");
        for (std::size_t index = 0; index < argumentCount; ++index)
            if (!matchesKind(
                    descriptor.parameterKinds[index], args[index].kind))
                return callbackTrap(
                    "invalid Xahau Hook callback argument type");

        auto* invocation = invocationFrom(caller);
        if (!invocation)
            return callbackTrap("missing Xahau Hook invocation context");
        if (descriptor.amendment != ripple::uint256{} &&
            !invocation->hookCtx.applyCtx.view().rules().enabled(
                descriptor.amendment))
            return ordinaryUnavailableResult(descriptor, results, resultCount);
        if (!resolved->binding)
            return callbackTrap("missing required Xahau Hook v1 binding");

        QuickJSHostCall call{*invocation, caller};
        auto const result =
            resolved->binding->invoke(call, std::span{args, argumentCount});
        if (call.fault())
            return callbackTrap(call.fault());
        return finishCallback(
            *invocation, descriptor, result, results, resultCount);
    }
    catch (...)
    {
        return callbackTrap("uncaught Xahau Hook host exception");
    }
}

}  // namespace hook::quickjs
