#ifndef XRPLD_APP_HOOK_HOOKHOSTOPERATIONRESULT_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTOPERATIONRESULT_H_INCLUDED

#include <xrpl/hook/Enum.h>
#include <cstdint>
#include <variant>

namespace hook {

using HookHostOperationResult =
    std::variant<std::uint64_t, hook_api::hook_return_code>;

namespace raw {
using Result = HookHostOperationResult;
}  // namespace raw
}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKHOSTOPERATIONRESULT_H_INCLUDED
