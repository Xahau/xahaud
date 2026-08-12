#include <xrpld/app/hook/HookHostOperations/Emission.h>
#include <xrpld/app/hook/applyHook.h>
#include <algorithm>

namespace hook::raw {
using enum hook_api::hook_return_code;

Result
etxnReserve(HookContext& hookCtx, std::uint32_t count)
{
    auto const result = hookCtx.api().etxn_reserve(count);
    if (!result)
        return result.error();
    return result.value();
}

Result
prepare(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength,
    std::uint32_t readPtr,
    std::uint32_t readLength)
{
    auto const source = memory.read(readPtr, readLength);
    if (!source || !memory.contains(writePtr, writeLength))
        return OUT_OF_BOUNDS;

    auto const result = hookCtx.api().prepare(ripple::Slice{
        static_cast<void const*>(source->data()), source->size()});
    if (!result)
        return result.error();
    auto const& bytes = result.value();

    // The QuickJS adapter uses fixed-buffer semantics. The legacy C wrapper
    // remains separate and preserves its historical actual-size copy.
    if (bytes.size() > writeLength)
        return TOO_SMALL;
    auto const destination =
        memory.write(writePtr, static_cast<std::uint32_t>(bytes.size()));
    if (!destination)
        return destination.error();
    std::copy(bytes.begin(), bytes.end(), destination->begin());
    return static_cast<std::uint64_t>(bytes.size());
}

Result
emit(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t writePtr,
    std::uint32_t writeLength,
    std::uint32_t readPtr,
    std::uint32_t readLength)
{
    auto const source = memory.read(readPtr, readLength);
    if (!source || !memory.contains(writePtr, writeLength))
        return OUT_OF_BOUNDS;
    if (writeLength < 32)
        return TOO_SMALL;

    auto const result = hookCtx.api().emit(ripple::Slice{
        static_cast<void const*>(source->data()), source->size()});
    if (!result)
        return result.error();

    auto const& transaction = *result;
    auto const& id = transaction->getID();
    if (id.size() > writeLength)
        return TOO_SMALL;
    auto const destination =
        memory.write(writePtr, static_cast<std::uint32_t>(id.size()));
    if (!destination)
        return destination.error();
    if (!memory.legacyWrite(
            writePtr, std::span<std::uint8_t const>{id.data(), id.size()}))
        return INTERNAL_ERROR;
    if (id.size() == 32)
        hookCtx.result.emittedTxn.push(transaction);
    return static_cast<std::uint64_t>(id.size());
}

}  // namespace hook::raw
