#include <xrpld/app/hook/HookHostOperations/State.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpl/hook/Misc.h>
#include <xrpl/protocol/Feature.h>
#include <algorithm>
#include <array>
#include <optional>

namespace hook::raw::v1 {
using enum hook_api::hook_return_code;
namespace {

std::optional<ripple::uint256>
makeStateKey(std::span<std::uint8_t const> source)
{
    if (source.empty() || source.size() > 32)
        return std::nullopt;
    std::array<std::uint8_t, 32> buffer{};
    std::copy(source.begin(), source.end(), buffer.end() - source.size());
    return ripple::uint256::fromVoid(buffer.data());
}

Result
dataAsInt64(std::span<std::uint8_t const> bytes)
{
    if (bytes.size() > 8)
        return TOO_BIG;
    std::uint64_t result = 0;
    for (auto byte : bytes)
        result = (result << 8) | byte;
    if ((std::uint64_t{1} << 63) & result)
        return TOO_BIG;
    return result;
}

}  // namespace

Result
state(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength,
    std::uint32_t keyPtr,
    std::uint32_t keyLength)
{
    if (keyLength > 32)
        return TOO_BIG;
    if (keyLength < 1)
        return TOO_SMALL;
    if (writeLength < 1 && writePtr != 0)
        return TOO_SMALL;

    auto const keyBytes = memory.read(keyPtr, keyLength);
    auto const destination = memory.write(writePtr, writeLength);
    if (!keyBytes || !destination)
        return OUT_OF_BOUNDS;
    auto const key = makeStateKey(*keyBytes);
    if (!key)
        return INVALID_ARGUMENT;

    auto const result = hookCtx.api().state_foreign(
        *key, hookCtx.result.hookNamespace, hookCtx.result.account);
    if (!result)
        return result.error();
    auto const& bytes = result.value();
    if (bytes.empty())
        return std::uint64_t{0};
    if (writePtr == 0)
        return dataAsInt64(bytes);
    if (bytes.size() > writeLength)
        return TOO_SMALL;
    if (!memory.legacyWrite(writePtr, bytes))
        return INTERNAL_ERROR;
    return static_cast<std::uint64_t>(bytes.size());
}

Result
stateSet(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t readPtr,
    std::uint32_t readLength,
    std::uint32_t keyPtr,
    std::uint32_t keyLength)
{
    if (!(readPtr == 0 && readLength == 0) &&
        !memory.contains(readPtr, readLength))
        return OUT_OF_BOUNDS;
    if (keyLength > 32)
        return TOO_BIG;
    if (keyLength < 1)
        return TOO_SMALL;
    auto const keyBytes = memory.read(keyPtr, keyLength);
    if (!keyBytes)
        return keyBytes.error();

    auto& view = hookCtx.applyCtx.view();
    auto const accountSLE = view.peek(hookCtx.result.accountKeylet);
    if (!accountSLE && view.rules().enabled(ripple::featureExtendedHookState))
        return static_cast<hook_api::hook_return_code>(ripple::tefINTERNAL);
    auto const scale = accountSLE->isFieldPresent(ripple::sfHookStateScale)
        ? accountSLE->getFieldU16(ripple::sfHookStateScale)
        : 1;
    if (readLength > hook::maxHookStateDataSize(scale))
        return TOO_BIG;

    if (view.rules().enabled(ripple::fixXahauV1) && !accountSLE)
        return static_cast<hook_api::hook_return_code>(ripple::tefINTERNAL);
    auto const key = makeStateKey(*keyBytes);
    if (!key)
        return INTERNAL_ERROR;

    ripple::Blob data;
    if (readLength != 0)
    {
        auto const bytes = memory.read(readPtr, readLength);
        if (!bytes)
            return bytes.error();
        data.assign(bytes->begin(), bytes->end());
    }
    auto const result = hookCtx.api().state_foreign_set(
        *key, hookCtx.result.hookNamespace, hookCtx.result.account, data);
    if (!result)
        return result.error();
    return result.value();
}

}  // namespace hook::raw::v1
