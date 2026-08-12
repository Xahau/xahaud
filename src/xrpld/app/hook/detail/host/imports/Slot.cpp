#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/host/imports/HookHostImportHelpers.h>

using namespace ripple;
using hook::Bytes;

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (write_ptr == 0)
    {
        // in this mode the function returns the data encoded in an int64_t
        if (write_len != 0)
            return INVALID_ARGUMENT;
    }
    else
    {
        if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
            return OUT_OF_BOUNDS;

        if (write_len < 1)
            return TOO_SMALL;
    }

    auto const result = api.slot(slot_no);
    if (!result)
        return result.error();

    Serializer s;
    (*result)->add(s);

    WRITE_WASM_MEMORY_OR_RETURN_AS_INT64(
        write_ptr,
        write_len,
        s.getDataPtr(),
        s.getDataLength(),
        (*result)->getSType() == STI_ACCOUNT);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, slot_clear, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.slot_clear(slot_no);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, slot_count, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.slot_count(slot_no);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_set,
    uint32_t read_ptr,
    uint32_t read_len,  // readptr is a keylet
    uint32_t slot_into /* providing 0 allocates a slot to you */)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes data{memory + read_ptr, memory + read_ptr + read_len};
    auto const result = api.slot_set(data, slot_into);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, slot_size, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.slot_size(slot_no);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_subarray,
    uint32_t parent_slot,
    uint32_t array_id,
    uint32_t new_slot)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.slot_subarray(parent_slot, array_id, new_slot);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_subfield,
    uint32_t parent_slot,
    uint32_t field_id,
    uint32_t new_slot)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.slot_subfield(parent_slot, field_id, new_slot);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, slot_type, uint32_t slot_no, uint32_t flags)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.slot_type(slot_no, flags);
    if (!result)
        return result.error();

    if (flags == 0)
    {
        auto const base = std::get<0>(*result);
        return static_cast<uint64_t>(base.getFName().fieldCode);
    }
    else
    {
        auto const amount = std::get<1>(*result);
        return amount.native();
    }

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, slot_float, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.slot_float(slot_no);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, meta_slot, uint32_t slot_into)
{
    HOOK_SETUP();

    auto const result = api.meta_slot(slot_into);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    xpop_slot,
    uint32_t slot_into_tx,
    uint32_t slot_into_meta)
{
    HOOK_SETUP();

    auto const result = api.xpop_slot(slot_into_tx, slot_into_meta);
    if (!result)
        return result.error();

    return std::get<0>(result.value()) << 16U | std::get<1>(result.value());

    HOOK_TEARDOWN();
}
