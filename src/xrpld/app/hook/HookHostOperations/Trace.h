#ifndef XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_TRACE_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_TRACE_H_INCLUDED

#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpld/app/hook/HookHostOperationResult.h>
#include <xrpl/beast/utility/Journal.h>
#include <cstdint>

namespace hook {
struct HookContext;

namespace raw {
Result
trace(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t messagePtr,
    std::uint32_t messageLength,
    std::uint32_t dataPtr,
    std::uint32_t dataLength,
    std::uint32_t asHex,
    beast::Journal const& journal);
}  // namespace raw
}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_TRACE_H_INCLUDED
