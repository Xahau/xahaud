#ifndef XRPLD_APP_HOOK_DETAIL_HOST_IMPORTS_HOOKHOSTIMPORTHELPERS_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_HOST_IMPORTS_HOOKHOSTIMPORTHELPERS_H_INCLUDED

#include <xrpl/hook/Enum.h>
#include <xrpl/protocol/Keylet.h>
#include <xrpl/protocol/UintTypes.h>

#include <cstdint>
#include <optional>
#include <variant>

std::optional<ripple::Currency>
parseCurrency(std::uint8_t* currency, std::uint32_t length);

std::variant<std::uint64_t, hook_api::hook_return_code>
serialize_keylet(
    ripple::Keylet& keylet,
    std::uint8_t* memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength);

std::optional<ripple::Keylet>
unserialize_keylet(std::uint8_t* bytes, std::uint32_t length);

std::variant<std::uint64_t, hook_api::hook_return_code>
data_as_int64(void const* bytes, std::uint32_t length);

#endif  // XRPLD_APP_HOOK_DETAIL_HOST_IMPORTS_HOOKHOSTIMPORTHELPERS_H_INCLUDED
