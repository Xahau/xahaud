#include <xrpld/app/hook/HookHostOperations.h>
#include <xrpld/app/hook/applyHook.h>

using namespace ripple;
using hook::Bytes;

// When implemented will return the hash of the current hook
DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_hash,
    uint32_t write_ptr,
    uint32_t write_len,
    int32_t hook_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (write_len < 32)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    auto const result = api.hook_hash(hook_no);
    if (!result)
        return result.error();
    auto const& hash = result.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, hash.data(), hash.size(), memory, memory_length);

    HOOK_TEARDOWN();
}
// Write the account id that the running hook is installed on into write_ptr
DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_account,
    uint32_t write_ptr,
    uint32_t ptr_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack
    return hook::raw::hookAccount(hookCtx, guestMemory, write_ptr, ptr_len);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_param,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes paramName{read_ptr + memory, read_ptr + read_len + memory};

    auto const result = api.hook_param(paramName);

    if (!result)
        return result.error();

    auto const& val = result.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, val.data(), val.size(), memory, memory_length);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_param_set,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t kread_ptr,
    uint32_t kread_len,
    uint32_t hread_ptr,
    uint32_t hread_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length) ||
        NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length) ||
        NOT_IN_BOUNDS(hread_ptr, hread_len, memory_length))
        return OUT_OF_BOUNDS;

    {
        // those checks are also done in the HookAPI
        // but we need to check them here too for backwards compatibility
        if (kread_len < 1)
            return TOO_SMALL;

        if (kread_len > hook::maxHookParameterKeySize())
            return TOO_BIG;

        if (hread_len != 32)
            return INVALID_ARGUMENT;

        if (read_len > hook::maxHookParameterValueSize())
            return TOO_BIG;
    }

    Bytes paramName{kread_ptr + memory, kread_ptr + kread_len + memory};
    Bytes paramValue{read_ptr + memory, read_ptr + read_len + memory};
    ripple::uint256 hash = ripple::uint256::fromVoid(memory + hread_ptr);

    auto const result = api.hook_param_set(hash, paramName, paramValue);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_skip,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t flags)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (read_len != 32)
        return INVALID_ARGUMENT;

    ripple::uint256 hash = ripple::uint256::fromVoid(memory + read_ptr);

    auto const result = api.hook_skip(hash, flags);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, hook_pos)
{
    return hookCtx.api().hook_pos();
}
DEFINE_HOOK_FUNCTION(int64_t, hook_again)
{
    HOOK_SETUP();

    auto const result = api.hook_again();

    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}
