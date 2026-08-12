#ifndef XRPLD_APP_HOOK_DETAIL_HOST_HOOKHOSTTEXT_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_HOST_HOOKHOSTTEXT_H_INCLUDED

#include <cstdint>
#include <span>

namespace hook::raw::detail {

inline bool
isUTF16LE(std::span<std::uint8_t const> bytes) noexcept
{
    if (bytes.empty() || bytes.size() % 2 != 0)
        return false;
    for (std::size_t index = 0; index < bytes.size(); index += 2)
        if (bytes[index] == 0 || bytes[index + 1] != 0)
            return false;
    return true;
}

}  // namespace hook::raw::detail

#endif  // XRPLD_APP_HOOK_DETAIL_HOST_HOOKHOSTTEXT_H_INCLUDED
