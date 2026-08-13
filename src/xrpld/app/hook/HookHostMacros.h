#ifndef XRPLD_APP_HOOK_HOOKHOSTMACROS_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTMACROS_H_INCLUDED

#include <xrpld/app/hook/HookHostFunction.h>
#include <xrpl/hook/Macro.h>
#include <bit>
#include <span>
#include <wasmedge/wasmedge.h>

namespace hook::detail {

inline bool
writeWasmEdgeGuestMemory(
    void* context,
    std::uint32_t offset,
    std::span<std::uint8_t const> bytes) noexcept
{
    auto* memory = static_cast<WasmEdge_MemoryInstanceContext*>(context);
    return memory &&
        WasmEdge_ResultOK(WasmEdge_MemoryInstanceSetData(
            memory, bytes.data(), offset, bytes.size()));
}

inline void
resolveWasmEdgeGuestMemory(
    void* context,
    std::uint8_t*& data,
    std::size_t& size,
    void*& writerContext,
    HookGuestMemory::CompatibilityWriter& writer) noexcept
{
    auto const* frame =
        static_cast<WasmEdge_CallingFrameContext const*>(context);
    auto* memoryContext = WasmEdge_CallingFrameGetMemoryInstance(frame, 0);
    data = WasmEdge_MemoryInstanceGetPointer(memoryContext, 0, 0);
    // Keep the legacy C-Hook 64-KiB Wasm page calculation.
    size = WasmEdge_MemoryInstanceGetPageSize(memoryContext) * 65536ULL;
    writerContext = memoryContext;
    writer = &writeWasmEdgeGuestMemory;
}

inline HookGuestMemory
wasmEdgeGuestMemory(WasmEdge_CallingFrameContext const& frame) noexcept
{
    return {
        const_cast<WasmEdge_CallingFrameContext*>(&frame),
        &resolveWasmEdgeGuestMemory,
        HookGuestMemory::WriteContract::legacyActualSize};
}

}  // namespace hook::detail

#undef DECLARE_HOOK_FUNCTION
#undef DEFINE_HOOK_FUNCTION
#undef HOOK_SETUP
#undef WRITE_WASM_MEMORY
#undef WRITE_WASM_MEMORY_AND_RETURN

#define HOST_VAL_uint32_t static_cast<uint32_t>(inputs[_hostStack++].asI32())
#define HOST_VAL_int32_t std::bit_cast<int32_t>(inputs[_hostStack++].asI32())
#define HOST_VAL_uint64_t static_cast<uint64_t>(inputs[_hostStack++].asI64())
#define HOST_VAL_int64_t std::bit_cast<int64_t>(inputs[_hostStack++].asI64())
#define HOST_KIND_uint32_t hook::HookHostValueKind::u32
#define HOST_KIND_int32_t hook::HookHostValueKind::i32
#define HOST_KIND_uint64_t hook::HookHostValueKind::u64
#define HOST_KIND_int64_t hook::HookHostValueKind::i64
#define HOST_VAR_ASSIGN(T, V)                          \
    if (inputs[_hostStack].kind != CAT(HOST_KIND_##T)) \
        return hook::HookHostCallStatus::trap;         \
    T V = CAT(HOST_VAL_##T)

#define HOST_RET_uint32_t(value) \
    hook::HookHostValue::u32(static_cast<uint32_t>(value))
#define HOST_RET_int32_t(value) \
    hook::HookHostValue::i32(static_cast<uint32_t>(value))
#define HOST_RET_uint64_t(value) \
    hook::HookHostValue::u64(static_cast<uint64_t>(value))
#define HOST_RET_int64_t(value) \
    hook::HookHostValue::i64(static_cast<uint64_t>(value))
#define HOST_RET_ASSIGN(T, value) CAT2(HOST_RET_, T(value))

#define DECLARE_HOOK_FUNCTION(R, F, ...)                                \
    std::variant<UNSIGNED_TYPE(R), hook_api::hook_return_code> F(       \
        hook::HookContext& hookCtx,                                     \
        hook::HookGuestMemory& frameCtx __VA_OPT__(COMMA __VA_ARGS__)); \
    extern WasmEdge_Result WasmFunction##F(                             \
        void* data_ptr,                                                 \
        WasmEdge_CallingFrameContext const* frameCtx,                   \
        WasmEdge_Value const* in,                                       \
        WasmEdge_Value* out);                                           \
    extern WasmEdge_ValType WasmFunctionParams##F[];                    \
    extern WasmEdge_ValType WasmFunctionResult##F[];                    \
    extern WasmEdge_FunctionTypeContext* WasmFunctionType##F;           \
    extern WasmEdge_String WasmFunctionName##F;                         \
    extern hook::HookHostCallStatus HostFunction##F(                    \
        void* userData,                                                 \
        hook::HookGuestMemory& guestMemory,                             \
        hook::HookHostValue const* inputs,                              \
        std::size_t inputCount,                                         \
        hook::HookHostValue* outputs,                                   \
        std::size_t outputCount);

#define DEFINE_HOOK_FUNCTION(R, F, ...)                                        \
    WasmEdge_Result hook_api::WasmFunction##F(                                 \
        void* data_ptr,                                                        \
        WasmEdge_CallingFrameContext const* frameCtx,                          \
        WasmEdge_Value const* in,                                              \
        WasmEdge_Value* out)                                                   \
    {                                                                          \
        __VA_OPT__(int _stack = 0;)                                            \
        __VA_OPT__(FOR_VARS(VAR_ASSIGN, 2, __VA_ARGS__);)                      \
        auto* hookCtx = reinterpret_cast<hook::HookContext*>(data_ptr);        \
        auto guestMemory = hook::detail::wasmEdgeGuestMemory(*frameCtx);       \
        auto const& return_code = hook_api::F(                                 \
            *hookCtx, guestMemory __VA_OPT__(COMMA STRIP_TYPES(__VA_ARGS__))); \
        if (std::holds_alternative<hook_api::hook_return_code>(return_code) && \
            (std::get<hook_api::hook_return_code>(return_code) ==              \
                 RC_ROLLBACK ||                                                \
             std::get<hook_api::hook_return_code>(return_code) == RC_ACCEPT))  \
            return WasmEdge_Result_Terminate;                                  \
        out[0] = RET_ASSIGN(                                                   \
            R,                                                                 \
            std::holds_alternative<UNSIGNED_TYPE(R)>(return_code)              \
                ? std::get<UNSIGNED_TYPE(R)>(return_code)                      \
                : R(std::get<hook_api::hook_return_code>(return_code)));       \
        return WasmEdge_Result_Success;                                        \
    }                                                                          \
    WasmEdge_ValType hook_api::WasmFunctionParams##F[] = {                     \
        __VA_OPT__(FOR_VARS(WASM_VAL_TYPE, 0, __VA_ARGS__))};                  \
    WasmEdge_ValType hook_api::WasmFunctionResult##F[1] = {                    \
        WASM_VAL_TYPE(R, dummy)};                                              \
    WasmEdge_FunctionTypeContext* hook_api::WasmFunctionType##F =              \
        WasmEdge_FunctionTypeCreate(                                           \
            WasmFunctionParams##F,                                             \
            VA_NARGS(NULL __VA_OPT__(, __VA_ARGS__)),                          \
            WasmFunctionResult##F,                                             \
            1);                                                                \
    WasmEdge_String hook_api::WasmFunctionName##F =                            \
        WasmEdge_StringCreateByCString(#F);                                    \
    hook::HookHostCallStatus hook_api::HostFunction##F(                        \
        void* userData,                                                        \
        hook::HookGuestMemory& guestMemory,                                    \
        hook::HookHostValue const* inputs,                                     \
        std::size_t inputCount,                                                \
        hook::HookHostValue* outputs,                                          \
        std::size_t outputCount)                                               \
    {                                                                          \
        constexpr auto expectedInputs =                                        \
            VA_NARGS(NULL __VA_OPT__(, __VA_ARGS__));                          \
        if (inputCount != expectedInputs || outputCount != 1 || !outputs ||    \
            (expectedInputs != 0 && !inputs))                                  \
            return hook::HookHostCallStatus::trap;                             \
        __VA_OPT__(int _hostStack = 0;)                                        \
        __VA_OPT__(FOR_VARS(HOST_VAR_ASSIGN, 2, __VA_ARGS__);)                 \
        auto* hookCtx = reinterpret_cast<hook::HookContext*>(userData);        \
        if (!hookCtx)                                                          \
            return hook::HookHostCallStatus::trap;                             \
        auto const& returnCode = hook_api::F(                                  \
            *hookCtx, guestMemory __VA_OPT__(COMMA STRIP_TYPES(__VA_ARGS__))); \
        if (std::holds_alternative<hook_api::hook_return_code>(returnCode) &&  \
            (std::get<hook_api::hook_return_code>(returnCode) ==               \
                 hook_api::hook_return_code::RC_ROLLBACK ||                    \
             std::get<hook_api::hook_return_code>(returnCode) ==               \
                 hook_api::hook_return_code::RC_ACCEPT))                       \
            return std::get<hook_api::hook_return_code>(returnCode) ==         \
                    hook_api::hook_return_code::RC_ACCEPT                      \
                ? hook::HookHostCallStatus::accept                             \
                : hook::HookHostCallStatus::rollback;                          \
        outputs[0] = HOST_RET_ASSIGN(                                          \
            R,                                                                 \
            std::holds_alternative<UNSIGNED_TYPE(R)>(returnCode)               \
                ? std::get<UNSIGNED_TYPE(R)>(returnCode)                       \
                : R(std::get<hook_api::hook_return_code>(returnCode)));        \
        return hook::HookHostCallStatus::success;                              \
    }                                                                          \
    std::variant<UNSIGNED_TYPE(R), hook_api::hook_return_code> hook_api::F(    \
        hook::HookContext& hookCtx,                                            \
        hook::HookGuestMemory& frameCtx __VA_OPT__(COMMA __VA_ARGS__))

#define HOOK_SETUP()                                                        \
    using enum hook_api::hook_return_code;                                  \
    try                                                                     \
    {                                                                       \
        [[maybe_unused]] ApplyContext& applyCtx = hookCtx.applyCtx;         \
        [[maybe_unused]] auto& view = applyCtx.view();                      \
        [[maybe_unused]] auto j = applyCtx.app.journal("View");             \
        frameCtx.resolve();                                                 \
        [[maybe_unused]] auto& guestMemory = frameCtx;                      \
        [[maybe_unused]] unsigned char* memory = guestMemory.data();        \
        [[maybe_unused]] const uint64_t memory_length = guestMemory.size(); \
        [[maybe_unused]] auto& api = hookCtx.api();                         \
        if (!guestMemory.valid())                                           \
            return INTERNAL_ERROR;

#define WRITE_WASM_MEMORY(                                                  \
    bytes_written,                                                          \
    guest_dst_ptr,                                                          \
    guest_dst_len,                                                          \
    host_src_ptr,                                                           \
    host_src_len,                                                           \
    host_memory_ptr,                                                        \
    guest_memory_length)                                                    \
    {                                                                       \
        int64_t bytes_to_write = std::min(                                  \
            static_cast<int64_t>(host_src_len),                             \
            static_cast<int64_t>(guest_dst_len));                           \
        if (guest_dst_ptr + bytes_to_write > guest_memory_length)           \
        {                                                                   \
            JLOG(j.warn()) << "HookError[" << HC_ACC() << "]: " << __func__ \
                           << " tried to retreive blob of " << host_src_len \
                           << " bytes past end of wasm memory";             \
            return OUT_OF_BOUNDS;                                           \
        }                                                                   \
        if (!guestMemory.compatibilityCopy(                                 \
                guest_dst_ptr,                                              \
                std::span<std::uint8_t const>{                              \
                    reinterpret_cast<uint8_t const*>(host_src_ptr),         \
                    static_cast<std::size_t>(bytes_to_write)}))             \
            return INTERNAL_ERROR;                                          \
        bytes_written += bytes_to_write;                                    \
    }

#define WRITE_WASM_MEMORY_AND_RETURN( \
    guest_dst_ptr,                    \
    guest_dst_len,                    \
    host_src_ptr,                     \
    host_src_len,                     \
    host_memory_ptr,                  \
    guest_memory_length)              \
    {                                 \
        uint64_t bytes_written = 0;   \
        WRITE_WASM_MEMORY(            \
            bytes_written,            \
            guest_dst_ptr,            \
            guest_dst_len,            \
            host_src_ptr,             \
            host_src_len,             \
            host_memory_ptr,          \
            guest_memory_length);     \
        return bytes_written;         \
    }

#endif  // XRPLD_APP_HOOK_HOOKHOSTMACROS_H_INCLUDED
