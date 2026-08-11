#include <xrpld/app/hook/HookHostOperations.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpl/hook/Misc.h>
#include <xrpl/protocol/Feature.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string_view>

namespace hook::raw {
using enum hook_api::hook_return_code;
namespace {

bool
isUTF16LE(std::span<std::uint8_t const> bytes)
{
    if (bytes.empty() || bytes.size() % 2 != 0)
        return false;
    for (std::size_t index = 0; index < bytes.size(); index += 2)
        if (bytes[index] == 0 || bytes[index + 1] != 0)
            return false;
    return true;
}

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
        if (isUTF16LE(*reason))
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

Result
trace(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t messagePtr,
    std::uint32_t messageLength,
    std::uint32_t dataPtr,
    std::uint32_t dataLength,
    std::uint32_t asHex,
    beast::Journal const& journal)
{
    auto message = memory.read(messagePtr, messageLength);
    auto data = memory.read(dataPtr, dataLength);
    if (!message || !data)
        return OUT_OF_BOUNDS;
    if (!journal.trace())
        return std::uint64_t{0};

    message = message->first(std::min<std::size_t>(message->size(), 128));
    data = data->first(std::min<std::size_t>(data->size(), 1023));

    std::array<std::uint8_t, 2200> output{};
    std::size_t length = 0;
    if (!message->empty())
    {
        std::copy(message->begin(), message->end(), output.begin());
        length = message->size();
        if (output[length - 1] == 0)
            --length;
        output[length++] = ':';
        output[length++] = ' ';
    }

    if (!data->empty())
    {
        if (asHex)
        {
            static constexpr char digits[] = "0123456789ABCDEF";
            for (auto byte : *data)
            {
                output[length++] = digits[byte >> 4];
                output[length++] = digits[byte & 0x0F];
            }
        }
        else if (isUTF16LE(*data))
        {
            for (std::size_t index = 0; index < data->size(); index += 2)
                output[length++] = (*data)[index];
        }
        else
        {
            std::copy(data->begin(), data->end(), output.begin() + length);
            length += data->size();
        }
    }

    if (length != 0)
        journal.trace() << "HookTrace[" << hookCtx.result.account << '-'
                        << hookCtx.result.otxnAccount << "]: "
                        << std::string_view{
                               reinterpret_cast<char const*>(output.data()),
                               length};
    return std::uint64_t{0};
}

Result
otxnType(HookContext& hookCtx)
{
    return hookCtx.api().otxn_type();
}

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
    std::copy_n(hash.data(), 32, destination->begin());
    return std::uint64_t{32};
}

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
    std::copy_n(account.data(), 20, destination->begin());
    return std::uint64_t{20};
}

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
    std::copy(bytes.begin(), bytes.end(), destination->begin());
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

}  // namespace hook::raw
