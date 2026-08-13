#include <xrpld/app/hook/HookHostOperations/HookContext.h>
#include <xrpld/app/hook/applyHook.h>
#include <algorithm>

namespace hook::raw::v1 {
using enum hook_api::hook_return_code;

Result
hookAccount(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength)
{
    auto destination = memory.write(writePtr, writeLength);
    if (!destination)
        return destination.error();
    if (writeLength < 20)
        return TOO_SMALL;
    auto const account = hookCtx.api().hook_account();
    if (!memory.compatibilityCopy(
            writePtr, std::span<std::uint8_t const>{account.data(), 20}))
        return INTERNAL_ERROR;
    return std::uint64_t{20};
}

}  // namespace hook::raw::v1
