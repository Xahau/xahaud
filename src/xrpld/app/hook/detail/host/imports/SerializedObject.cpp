#include <xrpld/app/hook/applyHook.h>

using namespace ripple;
using enum hook_api::hook_return_code;
using hook::Bytes;

/**
 * Check if any of the integer intervals overlap
 * [a,b,  c,d, ... ] ::== {a-b}, {c-d}, ...
 * TODO: naive implementation consider revising if
 * will be called with > 4 regions
 */
inline bool
overlapping_memory(std::vector<uint64_t> regions)
{
    for (uint64_t i = 0; i < regions.size() - 2; i += 2)
    {
        uint64_t a = regions[i + 0];
        uint64_t b = regions[i + 1];

        for (uint64_t j = i + 2; j < regions.size(); j += 2)
        {
            uint64_t c = regions[j + 0];
            uint64_t d = regions[j + 1];

            // only valid ways not to overlap are
            //
            // |===|  |===|
            // a   b  c   d
            //
            //      or
            // |===|  |===|
            // c   d  a   b

            if (d <= a || b <= c)
            {
                // no collision
                continue;
            }

            return true;
        }
    }

    return false;
}
// Given an serialized object in memory locate and return the offset and length
// of the payload of a subfield of that object. Arrays are returned fully
// formed. If successful returns offset and length joined as int64_t. Use
// SUB_OFFSET and SUB_LENGTH to extract.
DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_subfield,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t field_id)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes data{memory + read_ptr, memory + read_ptr + read_len};
    auto const result = api.sto_subfield(data, field_id);
    if (!result)
        return result.error();
    auto const& pair = result.value();
    return (uint64_t(pair.first) << 32U) + (uint32_t)pair.second;

    HOOK_TEARDOWN();
}
// Same as subfield but indexes into a serialized array
DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_subarray,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t index_id)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes data{memory + read_ptr, memory + read_ptr + read_len};
    auto const result = api.sto_subarray(data, index_id);
    if (!result)
        return result.error();
    auto const& pair = result.value();
    return (uint64_t(pair.first) << 32U) + (uint32_t)pair.second;

    HOOK_TEARDOWN();
}
/**
 * Inject a field into an sto if there is sufficient space
 * Field must be fully formed and wrapped (NOT JUST PAYLOAD)
 * sread - source object
 * fread - field to inject
 */
DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_emplace,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t sread_ptr,
    uint32_t sread_len,
    uint32_t fread_ptr,
    uint32_t fread_len,
    uint32_t field_id)
{
    HOOK_SETUP();

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(sread_ptr, sread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(fread_ptr, fread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_len < sread_len + fread_len)
        return TOO_SMALL;

    // RH TODO: put these constants somewhere (votable?)
    if (sread_len > 1024 * 16)
        return TOO_BIG;

    if (sread_len < 2)
        return TOO_SMALL;

    if (fread_len == 0 && fread_ptr == 0)
    {
        // this is a delete operation
        if (overlapping_memory(
                {write_ptr,
                 write_ptr + write_len,
                 sread_ptr,
                 sread_ptr + sread_len}))
            return MEM_OVERLAP;
    }
    else
    {
        if (fread_len > 4096)
            return TOO_BIG;

        if (fread_len < 2)
            return TOO_SMALL;

        // check for buffer overlaps
        if (overlapping_memory(
                {write_ptr,
                 write_ptr + write_len,
                 sread_ptr,
                 sread_ptr + sread_len,
                 fread_ptr,
                 fread_ptr + fread_len}))
            return MEM_OVERLAP;
    }

    Bytes source{memory + sread_ptr, memory + sread_ptr + sread_len};
    std::optional<Bytes> field;
    if (fread_len > 0 && fread_ptr > 0)
        field = Bytes{memory + fread_ptr, memory + fread_ptr + fread_len};
    auto const result = api.sto_emplace(source, field, field_id);
    if (!result)
        return result.error();
    auto const& bytes = result.value();

    if (bytes.size() > write_len)
        return INTERNAL_ERROR;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr,
        write_len,
        bytes.data(),
        bytes.size(),
        memory,
        memory_length);

    HOOK_TEARDOWN();
}
/**
 * Remove a field from an sto if the field is present
 */
DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_erase,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t field_id)
{
    // proxy only no setup or teardown
    auto ret = sto_emplace(
        hookCtx,
        guestMemory,
        write_ptr,
        write_len,
        read_ptr,
        read_len,
        0,
        0,
        field_id);

    if (std::holds_alternative<uint64_t>(ret))
    {
        auto const value = std::get<uint64_t>(ret);
        if (value > 0 && value == read_len)
            return DOESNT_EXIST;
    }

    return ret;
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_validate,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    // RH TODO: see if an internal ripple function/class would do this better

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes data{read_ptr + memory, read_ptr + read_len + memory};
    auto const result = api.sto_validate(data);
    if (!result)
        return result.error();
    return result.value() ? 1ULL : 0ULL;

    HOOK_TEARDOWN();
}
