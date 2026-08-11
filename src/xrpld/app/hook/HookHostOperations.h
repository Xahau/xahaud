#ifndef XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_H_INCLUDED

#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpl/beast/utility/Journal.h>
#include <cstdint>
#include <variant>

namespace hook {

struct HookContext;

namespace raw {

using Result = std::variant<std::uint64_t, hook_api::hook_return_code>;

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

Result
otxnType(HookContext& hookCtx);

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

Result
hookAccount(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength);

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

}  // namespace raw
}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_H_INCLUDED
