#ifndef XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_CONTROL_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_CONTROL_H_INCLUDED

#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpld/app/hook/HookHostOperationResult.h>
#include <cstdint>

namespace hook {
struct HookContext;

namespace raw::v1 {
Result
accept(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t readPtr,
    std::uint32_t readLength,
    std::int64_t code);

Result
rollback(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t readPtr,
    std::uint32_t readLength,
    std::int64_t code);
}  // namespace raw::v1
}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_CONTROL_H_INCLUDED
