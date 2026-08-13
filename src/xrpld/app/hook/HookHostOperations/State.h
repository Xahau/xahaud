#ifndef XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_STATE_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_STATE_H_INCLUDED

#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpld/app/hook/HookHostOperationResult.h>
#include <cstdint>

namespace hook {
struct HookContext;

namespace raw::v1 {
Result
state(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength,
    std::uint32_t keyPtr,
    std::uint32_t keyLength);

Result
stateSet(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t readPtr,
    std::uint32_t readLength,
    std::uint32_t keyPtr,
    std::uint32_t keyLength);
}  // namespace raw::v1
}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_STATE_H_INCLUDED
