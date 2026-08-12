#include <xrpld/app/hook/HookHostOperations/Control.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/host/HookHostText.h>
#include <algorithm>
#include <string>

namespace hook::raw {
using enum hook_api::hook_return_code;
namespace {

Result
terminal(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t readPtr,
    std::uint32_t readLength,
    std::int64_t code,
    hook_api::ExitType type)
{
    readLength = std::min<std::uint32_t>(readLength, 256);
    if (readPtr != 0)
    {
        auto const reason = memory.read(readPtr, readLength);
        if (!reason)
            return reason.error();
        if (detail::isUTF16LE(*reason))
        {
            std::string converted;
            converted.reserve(reason->size() / 2);
            for (std::size_t index = 0; index < reason->size(); index += 2)
                converted.push_back(static_cast<char>((*reason)[index]));
            hookCtx.result.exitReason = std::move(converted);
        }
        else
        {
            hookCtx.result.exitReason.assign(
                reinterpret_cast<char const*>(reason->data()), reason->size());
        }
    }
    hookCtx.result.exitType = type;
    hookCtx.result.exitCode = code;
    return type == hook_api::ExitType::ACCEPT ? RC_ACCEPT : RC_ROLLBACK;
}

}  // namespace

Result
accept(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t readPtr,
    std::uint32_t readLength,
    std::int64_t code)
{
    return terminal(
        hookCtx, memory, readPtr, readLength, code, hook_api::ExitType::ACCEPT);
}

Result
rollback(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t readPtr,
    std::uint32_t readLength,
    std::int64_t code)
{
    return terminal(
        hookCtx,
        memory,
        readPtr,
        readLength,
        code,
        hook_api::ExitType::ROLLBACK);
}

}  // namespace hook::raw
