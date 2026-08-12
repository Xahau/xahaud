#include <xrpld/app/hook/HookHostOperations/Trace.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <array>

namespace hook::quickjs {
namespace {

raw::Result
traceHandler(
    QuickJSHostCall& call,
    std::uint32_t messagePtr,
    std::uint32_t messageLength,
    std::uint32_t dataPtr,
    std::uint32_t dataLength,
    std::uint32_t asHex)
{
    auto memory = call.memory();
    if (!memory)
        return hook_api::hook_return_code::INTERNAL_ERROR;
    return raw::trace(
        call.hookContext(),
        *memory,
        messagePtr,
        messageLength,
        dataPtr,
        dataLength,
        asHex,
        call.journal());
}

std::uint64_t
traceMeasure(
    std::uint32_t,
    std::uint32_t messageLength,
    std::uint32_t,
    std::uint32_t dataLength,
    std::uint32_t) noexcept
{
    return static_cast<std::uint64_t>(messageLength) + dataLength;
}

constexpr std::array bindings{
    makeBinding<QuickJSImportId::trace, traceHandler, traceMeasure>()};

}  // namespace

std::span<QuickJSImportBinding const>
traceBindings() noexcept
{
    return bindings;
}

}  // namespace hook::quickjs
