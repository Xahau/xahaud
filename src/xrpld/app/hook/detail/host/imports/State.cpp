#include <xrpld/app/hook/HookHostOperations.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/host/imports/HookHostImportHelpers.h>

using namespace ripple;

// zero pad on the left a key to bring it up to 32 bytes
std::optional<ripple::uint256> inline make_state_key(std::string_view source)
{
    size_t source_len = source.size();

    if (source_len > 32 || source_len < 1)
        return std::nullopt;

    unsigned char key_buffer[32];
    int i = 0;
    int pad = 32 - source_len;

    // zero pad on the left
    for (; i < pad; ++i)
        key_buffer[i] = 0;

    const char* data = source.data();

    for (; i < 32; ++i)
        key_buffer[i] = data[i - pad];

    return ripple::uint256::fromVoid(key_buffer);
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    state_set,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t kread_ptr,
    uint32_t kread_len)
{
    HOOK_SETUP();
    return hook::raw::stateSet(
        hookCtx,
        hook::HookGuestMemory{memory, memory_length},
        read_ptr,
        read_len,
        kread_ptr,
        kread_len);
    HOOK_TEARDOWN();
}
// update or create a hook state object
// read_ptr = data to set, kread_ptr = key
// RH NOTE passing 0 size causes a delete operation which is as-intended
/*
    uint32_t write_ptr, uint32_t write_len,
    uint32_t kread_ptr, uint32_t kread_len,         // key
    uint32_t nread_ptr, uint32_t nread_len,         // namespace
    uint32_t aread_ptr, uint32_t aread_len )        // account
 */
DEFINE_HOOK_FUNCTION(
    int64_t,
    state_foreign_set,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t kread_ptr,
    uint32_t kread_len,
    uint32_t nread_ptr,
    uint32_t nread_len,
    uint32_t aread_ptr,
    uint32_t aread_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (read_ptr == 0 && read_len == 0)
    {
        // valid, this is a delete operation
    }
    else if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (kread_len > 32)
        return TOO_BIG;

    if (kread_len < 1)
        return TOO_SMALL;

    if (nread_len != 0 && nread_len != 32)
        return INVALID_ARGUMENT;

    if (aread_len != 0 && aread_len != 20)
        return INVALID_ARGUMENT;

    if (NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length))
        return OUT_OF_BOUNDS;

    // ns can be null if and only if this is a local set
    if (nread_ptr == 0 && nread_len == 0 && !(aread_ptr == 0 && aread_len == 0))
        return INVALID_ARGUMENT;

    if ((nread_len && NOT_IN_BOUNDS(nread_ptr, nread_len, memory_length)) ||
        (kread_len && NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length)) ||
        (aread_len && NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length)))
        return OUT_OF_BOUNDS;

    auto const sleAccount = view.peek(hookCtx.result.accountKeylet);
    if (!sleAccount && view.rules().enabled(featureExtendedHookState))
        // should return hook_api::hook_return_code
        return static_cast<hook_api::hook_return_code>(tefINTERNAL);

    uint16_t const hookStateScale = sleAccount->isFieldPresent(sfHookStateScale)
        ? sleAccount->getFieldU16(sfHookStateScale)
        : 1;

    uint32_t maxSize = hook::maxHookStateDataSize(hookStateScale);
    if (read_len > maxSize)
        return TOO_BIG;

    uint256 ns = nread_len == 0 ? hookCtx.result.hookNamespace
                                : uint256::fromVoid(memory + nread_ptr);

    ripple::AccountID acc = aread_len == 20
        ? AccountID::fromVoid(memory + aread_ptr)
        : hookCtx.result.account;

    auto const key = make_state_key(
        std::string_view{(const char*)(memory + kread_ptr), (size_t)kread_len});

    if (view.rules().enabled(fixXahauV1))
    {
        auto const sleAccount = view.peek(hookCtx.result.accountKeylet);
        if (!sleAccount)
            // should return hook_api::hook_return_code
            return static_cast<hook_api::hook_return_code>(tefINTERNAL);
    }

    if (!key)
        return INTERNAL_ERROR;

    ripple::Blob data{memory + read_ptr, memory + read_ptr + read_len};

    auto const result = api.state_foreign_set(*key, ns, acc, data);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
/* Retrieve the state into write_ptr identified by the key in kread_ptr */
DEFINE_HOOK_FUNCTION(
    int64_t,
    state,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t kread_ptr,
    uint32_t kread_len)
{
    HOOK_SETUP();
    return hook::raw::state(
        hookCtx, guestMemory, write_ptr, write_len, kread_ptr, kread_len);
    HOOK_TEARDOWN();
}
/* This api actually serves both local and foreign state requests
 * feeding aread_ptr = 0 and aread_len = 0 will cause it to read local
 * feeding nread_len = 0 will cause hook's native namespace to be used */
DEFINE_HOOK_FUNCTION(
    int64_t,
    state_foreign,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t kread_ptr,
    uint32_t kread_len,  // key
    uint32_t nread_ptr,
    uint32_t nread_len,  // namespace
    uint32_t aread_ptr,
    uint32_t aread_len)  // account
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    bool is_foreign = false;
    if (aread_ptr == 0)
    {
        // valid arguments, local state
        if (aread_len != 0)
            return INVALID_ARGUMENT;
    }
    else
    {
        // valid arguments, foreign state
        is_foreign = true;
        if (aread_len != 20)
            return INVALID_ARGUMENT;
    }

    if (kread_len > 32)
        return TOO_BIG;

    if (kread_len < 1)
        return TOO_SMALL;

    if (write_len < 1 && write_ptr != 0)
        return TOO_SMALL;

    if (!is_foreign && nread_len == 0)
    {
        // local account will be populated with local hook namespace unless
        // otherwise specified
    }
    else if (nread_len != 32)
        return INVALID_ARGUMENT;

    if (NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length) ||
        NOT_IN_BOUNDS(nread_ptr, nread_len, memory_length) ||
        NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length) ||
        NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    uint256 ns = nread_len == 0 ? hookCtx.result.hookNamespace
                                : uint256::fromVoid(memory + nread_ptr);

    ripple::AccountID acc = is_foreign ? AccountID::fromVoid(memory + aread_ptr)
                                       : hookCtx.result.account;

    auto const key = make_state_key(
        std::string_view{(const char*)(memory + kread_ptr), (size_t)kread_len});

    if (!key)
        return INVALID_ARGUMENT;

    auto const result = api.state_foreign(*key, ns, acc);
    if (!result)
        return result.error();
    auto const& b = result.value();

    WRITE_WASM_MEMORY_OR_RETURN_AS_INT64(
        write_ptr, write_len, b.data(), b.size(), false);

    HOOK_TEARDOWN();
}
