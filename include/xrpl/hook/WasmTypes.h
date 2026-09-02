#ifndef RIPPLE_HOOK_WASMTYPES_H_INCLUDED
#define RIPPLE_HOOK_WASMTYPES_H_INCLUDED

#include <xrpl/basics/base_uint.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hook {

struct GuestMemory
{
    uint8_t* base;
    uint64_t size;

    inline bool
    inBounds(uint64_t guestPtr, uint64_t len) const
    {
        if (len > 0 && guestPtr > (UINT64_MAX - len + 1))
            return false;
        return (guestPtr + len) <= size;
    }

    inline bool
    write(uint64_t guestPtr, void const* src, uint64_t len)
    {
        if (!inBounds(guestPtr, len))
            return false;
        std::memcpy(base + guestPtr, src, len);
        return true;
    }

    inline bool
    read(uint64_t guestPtr, void* dst, uint64_t len) const
    {
        if (!inBounds(guestPtr, len))
            return false;
        std::memcpy(dst, base + guestPtr, len);
        return true;
    }
};

struct WasmValue
{
    enum Kind : uint8_t { I32, I64 } kind;

    union
    {
        uint32_t u32;
        uint64_t u64;
    };

    static inline WasmValue
    i32(uint32_t v)
    {
        WasmValue w;
        w.kind = I32;
        w.u32 = v;
        return w;
    }

    static inline WasmValue
    i64(uint64_t v)
    {
        WasmValue w;
        w.kind = I64;
        w.u64 = v;
        return w;
    }

    inline uint32_t
    asI32() const
    {
        return u32;
    }

    inline uint64_t
    asI64() const
    {
        return u64;
    }
};

enum class HostCallStatus {
    Success,
    Terminate,
    Trap,
};

using HostFunctionFn = HostCallStatus (*)(
    void* userData,
    GuestMemory& mem,
    WasmValue const* in,
    size_t inLen,
    WasmValue* out,
    size_t outLen);

struct HostFunctionDecl
{
    char const* name;
    HostFunctionFn fn;
    std::vector<WasmValue::Kind> params;
    WasmValue::Kind result;
    ripple::uint256 const* featureGate;
};

}  // namespace hook

#endif  // RIPPLE_HOOK_WASMTYPES_H_INCLUDED
