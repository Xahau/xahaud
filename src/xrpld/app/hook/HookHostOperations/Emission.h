#ifndef XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_EMISSION_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_EMISSION_H_INCLUDED

#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpld/app/hook/HookHostOperationResult.h>
#include <cstdint>

namespace hook {
struct HookContext;

namespace raw {
Result
etxnReserve(HookContext& hookCtx, std::uint32_t count);

Result
prepare(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength,
    std::uint32_t readPtr,
    std::uint32_t readLength);

Result
emit(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength,
    std::uint32_t readPtr,
    std::uint32_t readLength);
}  // namespace raw
}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_EMISSION_H_INCLUDED
