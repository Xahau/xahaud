#ifndef XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_LEDGER_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_LEDGER_H_INCLUDED

#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpld/app/hook/HookHostOperationResult.h>
#include <cstdint>

namespace hook {
struct HookContext;

namespace raw {
Result
ledgerSequence(HookContext& hookCtx);

Result
ledgerLastTime(HookContext& hookCtx);

Result
ledgerLastHash(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength);
}  // namespace raw
}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_LEDGER_H_INCLUDED
