#include <xrpld/app/hook/HookHostOperations.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/host/imports/HookHostImportHelpers.h>

using namespace ripple;
using hook::Bytes;

// Write the TxnID of the originating transaction into the write_ptr
DEFINE_HOOK_FUNCTION(
    int64_t,
    otxn_id,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t flags)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.otxn_id(flags);
    if (!result)
        return result.error();

    auto const& txID = result.value();

    if (txID.size() > write_len)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, txID.size(), memory_length) ||
        NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr,
        txID.size(),
        txID.data(),
        txID.size(),
        memory,
        memory_length);

    HOOK_TEARDOWN();
}
// Return the tt (Transaction Type) numeric code of the originating transaction
DEFINE_HOOK_FUNCTION(int64_t, otxn_type)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    return hook::raw::otxnType(hookCtx);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, otxn_slot, uint32_t slot_into)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.otxn_slot(slot_into);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}
// Return the burden of the originating transaction... this will be 1 unless the
// originating transaction was itself an emitted transaction from a previous
// hook invocation
DEFINE_HOOK_FUNCTION(int64_t, otxn_burden)
{
    HOOK_SETUP();
    return api.otxn_burden();
    HOOK_TEARDOWN();
}
// Return the generation of the originating transaction... this will be 1 unless
// the originating transaction was itself an emitted transaction from a previous
// hook invocation
DEFINE_HOOK_FUNCTION(int64_t, otxn_generation)
{
    HOOK_SETUP();
    return api.otxn_generation();
    HOOK_TEARDOWN();
}
// Dump a field from the originating transaction into the hook's memory
DEFINE_HOOK_FUNCTION(
    int64_t,
    otxn_field,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t field_id)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (write_ptr == 0)
    {
        if (write_len != 0)
            return INVALID_ARGUMENT;

        // otherwise pass, we're trying to return the data as an int64_t
    }
    else if NOT_IN_BOUNDS (write_ptr, write_len, memory_length)
        return OUT_OF_BOUNDS;

    auto const result = api.otxn_field(field_id);
    if (!result)
        return result.error();

    auto const& field = result.value();

    Serializer s;
    field->add(s);

    WRITE_WASM_MEMORY_OR_RETURN_AS_INT64(
        write_ptr,
        write_len,
        s.getDataPtr(),
        s.getDataLength(),
        field->getSType() == STI_ACCOUNT);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    otxn_param,
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

    auto const result = api.otxn_param(paramName);
    if (!result)
        return result.error();
    auto const& val = result.value();

    if (val.size() > write_len)
        return TOO_SMALL;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, val.data(), val.size(), memory, memory_length);

    HOOK_TEARDOWN();
}
