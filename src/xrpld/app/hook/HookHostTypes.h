#ifndef XRPLD_APP_HOOK_HOOKHOSTTYPES_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTTYPES_H_INCLUDED

#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpl/basics/base_uint.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hook {

struct HookContext;

enum class HookHostValueKind : std::uint8_t { i32, i64 };

struct HookHostValue
{
    HookHostValueKind kind;
    union
    {
        std::uint32_t u32;
        std::uint64_t u64;
    };

    static HookHostValue
    i32(std::uint32_t value) noexcept
    {
        HookHostValue result;
        result.kind = HookHostValueKind::i32;
        result.u32 = value;
        return result;
    }

    static HookHostValue
    i64(std::uint64_t value) noexcept
    {
        HookHostValue result;
        result.kind = HookHostValueKind::i64;
        result.u64 = value;
        return result;
    }

    std::uint32_t
    asI32() const noexcept
    {
        return u32;
    }

    std::uint64_t
    asI64() const noexcept
    {
        return u64;
    }
};

enum class HookHostCallStatus : std::uint8_t { success, terminate, trap };

using HookHostFunction = HookHostCallStatus (*)(
    void* userData,
    HookGuestMemory& memory,
    HookHostValue const* inputs,
    std::size_t inputCount,
    HookHostValue* outputs,
    std::size_t outputCount);

struct HookHostFunctionDescriptor
{
    char const* name;
    HookHostFunction function;
    std::vector<HookHostValueKind> parameters;
    HookHostValueKind result;
    ripple::uint256 amendment;
};

std::vector<HookHostFunctionDescriptor> const&
hookHostFunctionCatalogue();

}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKHOSTTYPES_H_INCLUDED
