#ifndef XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_HOOKCONTEXT_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_HOOKCONTEXT_H_INCLUDED

#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpld/app/hook/HookHostOperationResult.h>
#include <cstdint>

namespace hook {
struct HookContext;

namespace raw {
Result
hookAccount(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength);
}  // namespace raw
}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_HOOKCONTEXT_H_INCLUDED
