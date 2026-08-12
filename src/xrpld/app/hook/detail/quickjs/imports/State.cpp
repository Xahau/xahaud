#include <xrpld/app/hook/HookHostOperations/State.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <array>

namespace hook::quickjs {
namespace {

raw::Result
stateHandler(
    QuickJSHostCall& call,
    std::uint32_t writePtr,
    std::uint32_t writeLength,
    std::uint32_t keyPtr,
    std::uint32_t keyLength)
{
    auto memory = call.memory();
    if (!memory)
        return hook_api::hook_return_code::INTERNAL_ERROR;
    return raw::state(
        call.hookContext(), *memory, writePtr, writeLength, keyPtr, keyLength);
}

raw::Result
stateSetHandler(
    QuickJSHostCall& call,
    std::uint32_t readPtr,
    std::uint32_t readLength,
    std::uint32_t keyPtr,
    std::uint32_t keyLength)
{
    auto memory = call.memory();
    if (!memory)
        return hook_api::hook_return_code::INTERNAL_ERROR;
    return raw::stateSet(
        call.hookContext(), *memory, readPtr, readLength, keyPtr, keyLength);
}

std::uint64_t
bufferPairMeasure(
    std::uint32_t,
    std::uint32_t firstLength,
    std::uint32_t,
    std::uint32_t secondLength) noexcept
{
    return static_cast<std::uint64_t>(firstLength) + secondLength;
}

constexpr std::array bindings{
    makeBinding<QuickJSImportId::state, stateHandler, bufferPairMeasure>(),
    makeBinding<
        QuickJSImportId::state_set,
        stateSetHandler,
        bufferPairMeasure>()};

}  // namespace

std::span<QuickJSImportBinding const>
stateBindings() noexcept
{
    return bindings;
}

}  // namespace hook::quickjs
