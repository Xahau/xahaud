#ifndef XRPLD_APP_HOOK_HOOKHOSTFUNCTION_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTFUNCTION_H_INCLUDED

#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpl/basics/base_uint.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace hook {

enum class HookHostValueKind : std::uint8_t { i32, u32, i64, u64 };

struct HookHostValue
{
    HookHostValueKind kind;
    union
    {
        std::uint32_t bits32;
        std::uint64_t bits64;
    };

    static HookHostValue
    i32(std::uint32_t value) noexcept
    {
        HookHostValue result;
        result.kind = HookHostValueKind::i32;
        result.bits32 = value;
        return result;
    }

    static HookHostValue
    u32(std::uint32_t value) noexcept
    {
        HookHostValue result;
        result.kind = HookHostValueKind::u32;
        result.bits32 = value;
        return result;
    }

    static HookHostValue
    i64(std::uint64_t value) noexcept
    {
        HookHostValue result;
        result.kind = HookHostValueKind::i64;
        result.bits64 = value;
        return result;
    }

    static HookHostValue
    u64(std::uint64_t value) noexcept
    {
        HookHostValue result;
        result.kind = HookHostValueKind::u64;
        result.bits64 = value;
        return result;
    }

    std::uint32_t
    asI32() const noexcept
    {
        return bits32;
    }

    std::uint64_t
    asI64() const noexcept
    {
        return bits64;
    }
};

enum class HookHostCallStatus : std::uint8_t {
    success,
    accept,
    rollback,
    trap
};

constexpr bool
isI32(HookHostValueKind kind) noexcept
{
    return kind == HookHostValueKind::i32 || kind == HookHostValueKind::u32;
}

using HookHostFunction = HookHostCallStatus (*)(
    void* userData,
    HookGuestMemory& memory,
    HookHostValue const* inputs,
    std::size_t inputCount,
    HookHostValue* outputs,
    std::size_t outputCount);

struct HookHostFunctionDescriptor
{
    std::string_view name;
    HookHostFunction function;
    std::vector<HookHostValueKind> parameters;
    HookHostValueKind result;
    ripple::uint256 declarationAmendment;
    std::uint16_t implementationVersion;
};

// Version 1 is a pre-activation identity for the current shared bodies. Before
// featureJSHooks becomes supported, activation must freeze genuinely retained
// versioned bodies; changing this tag alone is not historical retention.

std::span<HookHostFunctionDescriptor const>
hookHostFunctionCatalogue() noexcept;

HookHostFunctionDescriptor const*
findHookHostFunction(std::string_view name) noexcept;

HookHostFunctionDescriptor const*
findHookHostFunction(
    std::string_view name,
    std::uint16_t implementationVersion) noexcept;

}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKHOSTFUNCTION_H_INCLUDED
