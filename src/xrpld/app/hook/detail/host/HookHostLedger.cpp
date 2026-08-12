#include <xrpld/app/hook/HookHostOperations/Ledger.h>
#include <xrpld/app/hook/applyHook.h>
#include <algorithm>

namespace hook::raw {
using enum hook_api::hook_return_code;

Result
ledgerSequence(HookContext& hookCtx)
{
    return hookCtx.api().ledger_seq();
}

Result
ledgerLastTime(HookContext& hookCtx)
{
    return hookCtx.api().ledger_last_time();
}

Result
ledgerLastHash(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength)
{
    auto destination = memory.write(writePtr, writeLength);
    if (!destination)
        return destination.error();
    if (writeLength < 32)
        return TOO_SMALL;
    auto const hash = hookCtx.api().ledger_last_hash();
    if (!memory.legacyWrite(
            writePtr, std::span<std::uint8_t const>{hash.data(), 32}))
        return INTERNAL_ERROR;
    return std::uint64_t{32};
}

}  // namespace hook::raw
