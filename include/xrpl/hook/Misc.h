#ifndef HOOKMISC_INCLUDED
#define HOOKMISC_INCLUDED 1

#include <xrpl/basics/base_uint.h>

#include <array>
#include <cstdint>

namespace ripple {

/** UINT256_BIT[i] is the 256-bit value with only bit i set (LSB is bit 0).

    Used to encode and test the per-transaction-type bits of the HookOn and
    HookCanEmit fields. Built at compile time.
 */
inline constexpr auto UINT256_BIT = []() consteval {
    std::array<ripple::uint256, 256> table{};

    for (int i = 0; i < 256; ++i)
    {
        std::array<std::uint8_t, 32> bytes{};
        bytes[31 - (i / 8)] = 1 << (i % 8);
        table[i] = ripple::uint256(bytes);
    }

    return table;
}();

}  // namespace ripple

#endif
