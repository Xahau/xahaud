#include <xrpld/app/hook/HookHostOperations.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/host/imports/HookHostImportHelpers.h>

using namespace ripple;

// Return the current ledger sequence number
DEFINE_HOOK_FUNCTION(int64_t, ledger_seq)
{
    HOOK_SETUP();

    return hook::raw::ledgerSequence(hookCtx);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    ledger_last_hash,
    uint32_t write_ptr,
    uint32_t write_len)
{
    HOOK_SETUP();
    return hook::raw::ledgerLastHash(
        hookCtx, guestMemory, write_ptr, write_len);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, ledger_last_time)
{
    HOOK_SETUP();

    return hook::raw::ledgerLastTime(hookCtx);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    ledger_nonce,
    uint32_t write_ptr,
    uint32_t write_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx, view on current stack

    if (write_len < 32)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    auto const result = api.ledger_nonce();
    if (!result)
        return result.error();
    auto const& hash = result.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32, hash.data(), 32, memory, memory_length);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    ledger_keylet,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t lread_ptr,
    uint32_t lread_len,
    uint32_t hread_ptr,
    uint32_t hread_len)
{
    HOOK_SETUP();

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length) ||
        NOT_IN_BOUNDS(lread_ptr, lread_len, memory_length) ||
        NOT_IN_BOUNDS(hread_ptr, hread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (lread_len < 34U || hread_len < 34U || write_len < 34U)
        return TOO_SMALL;
    if (lread_len > 34U || hread_len > 34U || write_len > 34U)
        return TOO_BIG;

    std::optional<ripple::Keylet> klLo =
        unserialize_keylet(memory + lread_ptr, lread_len);
    if (!klLo)
        return INVALID_ARGUMENT;

    std::optional<ripple::Keylet> klHi =
        unserialize_keylet(memory + hread_ptr, hread_len);
    if (!klHi)
        return INVALID_ARGUMENT;

    auto const result = api.ledger_keylet(*klLo, *klHi);
    if (!result)
        return result.error();
    auto kl_out = result.value();

    return serialize_keylet(kl_out, memory, write_ptr, write_len);

    HOOK_TEARDOWN();
}
// Return the current fee base of the current ledger (multiplied by a margin)
DEFINE_HOOK_FUNCTION(int64_t, fee_base)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    return api.fee_base();

    HOOK_TEARDOWN();
}
