#include <xrpld/app/hook/detail/host/imports/HookHostImportHelpers.h>

using namespace ripple;
using enum hook_api::hook_return_code;

// cu_ptr is a pointer into memory, bounds check is assumed to have already
// happened
std::optional<Currency>
parseCurrency(uint8_t* cu_ptr, uint32_t cu_len)
{
    if (cu_len == 20)
    {
        // normal 20 byte currency
        return Currency::fromVoid(cu_ptr);
    }
    else if (cu_len == 3)
    {
        // 3 byte ascii currency
        // need to check what data is in these three bytes, to ensure ISO4217
        // compliance
        auto const validateChar = [](uint8_t c) -> bool {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '?' || c == '!' || c == '@' ||
                c == '#' || c == '$' || c == '%' || c == '^' || c == '&' ||
                c == '*' || c == '<' || c == '>' || c == '(' || c == ')' ||
                c == '{' || c == '}' || c == '[' || c == ']' || c == '|';
        };

        if (!validateChar(*((uint8_t*)(cu_ptr + 0U))) ||
            !validateChar(*((uint8_t*)(cu_ptr + 1U))) ||
            !validateChar(*((uint8_t*)(cu_ptr + 2U))))
            return {};

        uint8_t cur_buf[20] = {
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            *((uint8_t*)(cu_ptr + 0U)),
            *((uint8_t*)(cu_ptr + 1U)),
            *((uint8_t*)(cu_ptr + 2U)),
            0,
            0,
            0,
            0,
            0};
        return Currency::fromVoid(cur_buf);
    }
    else
        return {};
}
std::variant<uint64_t, hook_api::hook_return_code>
serialize_keylet(
    ripple::Keylet& kl,
    uint8_t* memory,
    uint32_t write_ptr,
    uint32_t write_len)
{
    if (write_len < 34)
        return TOO_SMALL;

    memory[write_ptr + 0] = (kl.type >> 8) & 0xFFU;
    memory[write_ptr + 1] = (kl.type >> 0) & 0xFFU;

    for (int i = 0; i < 32; ++i)
        memory[write_ptr + 2 + i] = kl.key.data()[i];

    return 34ULL;
}
std::optional<ripple::Keylet>
unserialize_keylet(uint8_t* ptr, uint32_t len)
{
    if (len != 34)
        return {};

    uint16_t ktype = ((uint16_t)ptr[0] << 8) + ((uint16_t)ptr[1]);

    return ripple::Keylet{
        static_cast<LedgerEntryType>(ktype),
        ripple::uint256::fromVoid(ptr + 2)};
}
// many datatypes can be encoded into an int64_t
std::variant<uint64_t, hook_api::hook_return_code>
data_as_int64(void const* ptr_raw, uint32_t len)
{
    if (len > 8)
        return TOO_BIG;

    uint8_t const* ptr = reinterpret_cast<uint8_t const*>(ptr_raw);
    uint64_t output = 0;
    for (int i = 0, j = (len - 1) * 8; i < len; ++i, j -= 8)
        output += (((uint64_t)ptr[i]) << j);
    if ((1ULL << 63U) & output)
        return TOO_BIG;
    return output;
}
