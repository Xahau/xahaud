#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/host/imports/HookHostImportHelpers.h>

#include <boost/multiprecision/cpp_dec_float.hpp>

using namespace ripple;
using namespace hook::hook_float;
using hook::Bytes;

#define RETURN_IF_INVALID_FLOAT(float1)                 \
    {                                                   \
        if (float1 < 0)                                 \
            return INVALID_FLOAT;                       \
        if (float1 != 0)                                \
        {                                               \
            auto const mantissa = get_mantissa(float1); \
            auto const exponent = get_exponent(float1); \
            if (!mantissa || !exponent)                 \
                return INVALID_FLOAT;                   \
            if (mantissa.value() < minMantissa ||       \
                mantissa.value() > maxMantissa ||       \
                exponent.value() > maxExponent ||       \
                exponent.value() < minExponent)         \
                return INVALID_FLOAT;                   \
        }                                               \
    }

DEFINE_HOOK_FUNCTION(int64_t, float_set, int32_t exp, int64_t mantissa)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.float_set(exp, mantissa);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    float_int,
    int64_t float1,
    uint32_t decimal_places,
    uint32_t absolute)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_int(float1, decimal_places, absolute);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, float_multiply, int64_t float1, int64_t float2)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    auto const result = api.float_multiply(float1, float2);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    float_mulratio,
    int64_t float1,
    uint32_t round_up,
    uint32_t numerator,
    uint32_t denominator)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result =
        api.float_mulratio(float1, round_up, numerator, denominator);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, float_negate, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    return api.float_negate(float1);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    float_compare,
    int64_t float1,
    int64_t float2,
    uint32_t mode)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    auto const result = api.float_compare(float1, float2, mode);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, float_sum, int64_t float1, int64_t float2)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    auto const result = api.float_sum(float1, float2);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    float_sto,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t cread_ptr,
    uint32_t cread_len,
    uint32_t iread_ptr,
    uint32_t iread_len,
    int64_t float1,
    uint32_t field_code)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    std::optional<Currency> currency;
    std::optional<AccountID> issuer;

    // bounds and argument checks
    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (cread_len == 0)
    {
        if (cread_ptr != 0)
            return INVALID_ARGUMENT;
    }
    else
    {
        if (cread_len != 20 && cread_len != 3)
            return INVALID_ARGUMENT;

        if (NOT_IN_BOUNDS(cread_ptr, cread_len, memory_length))
            return OUT_OF_BOUNDS;

        currency = parseCurrency(memory + cread_ptr, cread_len);

        if (!currency)
            return INVALID_ARGUMENT;
    }

    if (iread_len == 0)
    {
        if (iread_ptr != 0)
            return INVALID_ARGUMENT;
    }
    else
    {
        if (iread_len != 20)
            return INVALID_ARGUMENT;

        if (NOT_IN_BOUNDS(iread_ptr, iread_len, memory_length))
            return OUT_OF_BOUNDS;

        issuer = AccountID::fromVoid(memory + iread_ptr);
    }

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result =
        api.float_sto(currency, issuer, float1, field_code, write_len);
    if (!result)
        return result.error();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr,
        write_len,
        (*result).data(),
        (*result).size(),
        memory,
        memory_length);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    float_sto_set,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (read_len < 8)
        return NOT_AN_OBJECT;

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes data{read_ptr + memory, read_ptr + read_len + memory};

    auto const result = api.float_sto_set(data);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, float_divide, int64_t float1, int64_t float2)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    auto const result = api.float_divide(float1, float2);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, float_one)
{
    return hookCtx.api().float_one();
}
DEFINE_HOOK_FUNCTION(int64_t, float_invert, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_invert(float1);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, float_mantissa, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_mantissa(float1);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, float_sign, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    return api.float_sign(float1);

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, float_log, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_log(float1);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
DEFINE_HOOK_FUNCTION(int64_t, float_root, int64_t float1, uint32_t n)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_root(float1, n);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}
#undef RETURN_IF_INVALID_FLOAT
