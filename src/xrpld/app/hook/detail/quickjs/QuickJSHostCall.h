#ifndef XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSHOSTCALL_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSHOSTCALL_H_INCLUDED

#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpld/app/hook/HookHostFunction.h>
#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostPolicy.h>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <wasmtime.h>

namespace hook {
struct HookContext;

namespace quickjs {

std::uint64_t
quickJSHostWorkCost(
    QuickJSRuntimeProfile const& profile,
    std::uint64_t declaredBytes) noexcept;

struct QuickJSInvocation
{
    HookContext& hookCtx;
    QuickJSRuntimeProfile const& profile;
    std::uint64_t hostWorkRemaining;
#ifdef ENABLE_TESTS
    std::uint64_t observationInvocation;
#endif
    bool terminal = false;

    QuickJSInvocation(
        HookContext& hookCtx,
        QuickJSRuntimeProfile const& profile) noexcept;
};

class QuickJSHostCall
{
public:
    QuickJSHostCall(
        QuickJSInvocation& invocation,
        wasmtime_caller_t* caller) noexcept;

    std::optional<HookGuestMemory>
    memory() const noexcept;

    bool
    charge(std::uint64_t declaredBytes) noexcept;

private:
    QuickJSInvocation& invocation_;
    wasmtime_caller_t* caller_;
};

struct WasmtimeHostBinding
{
    std::string_view module;
    std::string_view name;
    std::array<HookHostValueKind, maxImportParameters> parameters;
    std::uint8_t parameterCount;
    HookHostValueKind result;
    std::uint16_t implementationVersion;
    HookHostFunctionDescriptor const* operation;
    ripple::uint256 amendment;
    HostWorkMeasureKind measure;
    TerminalBehavior terminal;
    enum class Charging : std::uint8_t { none, quickJSV1 } charging;
};

bool
validWasmtimeHostBinding(WasmtimeHostBinding const& binding) noexcept;

std::uint64_t
declaredHostWork(
    HostWorkMeasureKind measure,
    std::span<wasmtime_val_t const> values) noexcept;

wasm_trap_t*
rawHookCallback(
    void* environment,
    wasmtime_caller_t* caller,
    wasmtime_val_t const* args,
    std::size_t argumentCount,
    wasmtime_val_t* results,
    std::size_t resultCount) noexcept;

}  // namespace quickjs
}  // namespace hook

#endif  // XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSHOSTCALL_H_INCLUDED
