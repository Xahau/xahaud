#include <xrpld/app/hook/HookHostOperations.h>
#include <xrpld/app/hook/applyHook.h>

using namespace ripple;
using namespace hook::hook_float;

/* If XRPLD is running with trace log level hooks may produce debugging output
 * to the trace log specifying both a string and an integer to output */
DEFINE_HOOK_FUNCTION(
    int64_t,
    trace_num,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t number)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx on
                   // current stack
    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (!j.trace())
        return 0ULL;

    if (read_len > 128)
        read_len = 128;

    if (read_len > 0)
    {
        // skip \0 if present at the end
        if (*((const char*)memory + read_ptr + read_len - 1) == '\0')
            read_len--;

        if (read_len > 0)
        {
            j.trace() << "HookTrace[" << HC_ACC() << "]: "
                      << std::string_view(
                             (const char*)memory + read_ptr, read_len)
                      << ": " << number;

            return 0ULL;
        }
    }

    j.trace() << "HookTrace[" << HC_ACC() << "]: " << number;
    return 0ULL;
    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    trace,
    uint32_t mread_ptr,
    uint32_t mread_len,
    uint32_t dread_ptr,
    uint32_t dread_len,
    uint32_t as_hex)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx on
                   // current stack
    return hook::raw::trace(
        hookCtx,
        hook::HookGuestMemory{memory, memory_length},
        mread_ptr,
        mread_len,
        dread_ptr,
        dread_len,
        as_hex,
        j);
    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    trace_float,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx on
                   // current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (!j.trace())
        return 0ULL;

    if (read_len > 128)
        read_len = 128;

    // omit \0 if present
    if (read_len > 0 &&
        *((const char*)memory + read_ptr + read_len - 1) == '\0')
        read_len--;

    auto const messageKey = (read_len == 0)
        ? ""
        : std::string_view((const char*)memory + read_ptr, read_len);

    if (float1 == 0)
    {
        j.trace() << "HookTrace[" << HC_ACC() << "]: " << messageKey
                  << ": Float 0*10^(0) <ZERO>";
        return 0ULL;
    }

    auto const man = get_mantissa(float1);
    auto const exp = get_exponent(float1);
    bool neg = is_negative(float1);
    if (!man || !exp || man.value() < minMantissa ||
        man.value() > maxMantissa || exp.value() < minExponent ||
        exp.value() > maxExponent)
    {
        j.trace() << "HookTrace[" << HC_ACC() << "]: " << messageKey
                  << ": Float <INVALID>";
        return 0ULL;
    }

    j.trace() << "HookTrace[" << HC_ACC() << "]:" << messageKey << ": Float "
              << (neg ? "-" : "") << man.value() << "*10^(" << exp.value()
              << ")";
    return 0ULL;

    HOOK_TEARDOWN();
}
