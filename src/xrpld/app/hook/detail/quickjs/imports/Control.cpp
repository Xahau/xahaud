#include <xrpld/app/hook/HookHostOperations/Control.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <array>

namespace hook::quickjs {
namespace {

raw::Result
acceptHandler(
    QuickJSHostCall& call,
    std::uint32_t readPtr,
    std::uint32_t readLength,
    std::int64_t code)
{
    auto memory = call.memory();
    if (!memory)
        return hook_api::hook_return_code::INTERNAL_ERROR;
    return raw::v1::accept(
        call.hookContext(), *memory, readPtr, readLength, code);
}

std::uint64_t
terminalMeasure(std::uint32_t, std::uint32_t readLength, std::int64_t) noexcept
{
    return readLength;
}

raw::Result
rollbackHandler(
    QuickJSHostCall& call,
    std::uint32_t readPtr,
    std::uint32_t readLength,
    std::int64_t code)
{
    auto memory = call.memory();
    if (!memory)
        return hook_api::hook_return_code::INTERNAL_ERROR;
    return raw::v1::rollback(
        call.hookContext(), *memory, readPtr, readLength, code);
}

constexpr std::array bindings{
    makeBinding<QuickJSV1ImportId::accept, acceptHandler, terminalMeasure>(),
    makeBinding<
        QuickJSV1ImportId::rollback,
        rollbackHandler,
        terminalMeasure>()};

}  // namespace

std::span<QuickJSImportBinding const>
controlBindings() noexcept
{
    return bindings;
}

}  // namespace hook::quickjs
