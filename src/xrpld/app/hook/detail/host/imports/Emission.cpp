#include <xrpld/app/hook/HookHostOperations.h>
#include <xrpld/app/hook/applyHook.h>

using namespace ripple;
using hook::Bytes;

// Return the generation of a hypothetically emitted transaction from this hook
DEFINE_HOOK_FUNCTION(int64_t, etxn_generation)
{
    // proxy only, no setup or teardown
    return hookCtx.api().etxn_generation();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    prepare,
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

    ripple::Slice txBlob{
        reinterpret_cast<const void*>(memory + read_ptr), read_len};

    auto const res = api.prepare(txBlob);
    if (!res)
        return res.error();

    auto tx_blob = res.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr,
        tx_blob.size(),
        tx_blob.data(),
        tx_blob.size(),
        memory,
        memory_length);

    HOOK_TEARDOWN();
}
/* Emit a transaction from this hook. Transaction must be in STObject form,
 * fully formed and valid. XRPLD does not modify transactions it only checks
 * them for validity. */
DEFINE_HOOK_FUNCTION(
    int64_t,
    emit,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    return hook::raw::emit(
        hookCtx, guestMemory, write_ptr, write_len, read_ptr, read_len);

    HOOK_TEARDOWN();
}
// Deterministic nonces (can be called multiple times)
// Writes nonce into the write_ptr
DEFINE_HOOK_FUNCTION(
    int64_t,
    etxn_nonce,
    uint32_t write_ptr,
    uint32_t write_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx, view on current stack

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    // It is also checked in api.etxn_nonce, but for backwards compatibility, it
    // must be checked before the TOO_SMALL check.
    if (hookCtx.emit_nonce_counter > hook_api::max_nonce)
        return TOO_MANY_NONCES;

    if (write_len < 32)
        return TOO_SMALL;

    auto const result = api.etxn_nonce();
    if (!result)
        return result.error();
    auto const& hash = result.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32, hash.data(), 32, memory, memory_length);

    HOOK_TEARDOWN();
}
// Reserve one or more transactions for emission from the running hook
DEFINE_HOOK_FUNCTION(int64_t, etxn_reserve, uint32_t count)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    return hook::raw::etxnReserve(hookCtx, count);

    HOOK_TEARDOWN();
}
// Compute the burden of an emitted transaction based on a number of factors
DEFINE_HOOK_FUNCTION(int64_t, etxn_burden)
{
    HOOK_SETUP();
    auto const burden = api.etxn_burden();
    if (!burden)
        return burden.error();
    return burden.value();
    HOOK_TEARDOWN();
}
// Return the fee base for a hypothetically emitted transaction from the current
// hook based on byte count
DEFINE_HOOK_FUNCTION(
    int64_t,
    etxn_fee_base,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();
    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;
    ripple::Slice tx{
        reinterpret_cast<const void*>(read_ptr + memory), read_len};
    auto const fee_base = api.etxn_fee_base(tx);
    if (!fee_base)
        return fee_base.error();
    return fee_base.value();
    HOOK_TEARDOWN();
}
// Populate an sfEmitDetails field in a soon-to-be emitted transaction
DEFINE_HOOK_FUNCTION(
    int64_t,
    etxn_details,
    uint32_t write_ptr,
    uint32_t write_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    int64_t expected_size = 138U;
    if (!hookCtx.result.hasCallback)
        expected_size -= 22U;

    if (write_len < expected_size)
        return TOO_SMALL;

    auto const result = api.etxn_details(memory + write_ptr);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
