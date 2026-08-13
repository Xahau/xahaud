#include <xrpld/app/hook/HookHostOperations/Emission.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <array>

namespace hook::quickjs {
namespace {

raw::Result
etxnReserveHandler(QuickJSHostCall& call, std::uint32_t count)
{
    return raw::v1::etxnReserve(call.hookContext(), count);
}

std::uint64_t
scalarMeasure(std::uint32_t) noexcept
{
    return 0;
}

raw::Result
prepareHandler(
    QuickJSHostCall& call,
    std::uint32_t writePtr,
    std::uint32_t writeLength,
    std::uint32_t readPtr,
    std::uint32_t readLength)
{
    auto memory = call.memory();
    if (!memory)
        return hook_api::hook_return_code::INTERNAL_ERROR;
    return raw::v1::prepare(
        call.hookContext(),
        *memory,
        writePtr,
        writeLength,
        readPtr,
        readLength);
}

std::uint64_t
prepareMeasure(
    std::uint32_t,
    std::uint32_t writeLength,
    std::uint32_t,
    std::uint32_t readLength) noexcept
{
    return static_cast<std::uint64_t>(writeLength) + readLength;
}

raw::Result
emitHandler(
    QuickJSHostCall& call,
    std::uint32_t writePtr,
    std::uint32_t writeLength,
    std::uint32_t readPtr,
    std::uint32_t readLength)
{
    auto memory = call.memory();
    if (!memory)
        return hook_api::hook_return_code::INTERNAL_ERROR;
    return raw::v1::emit(
        call.hookContext(),
        *memory,
        writePtr,
        writeLength,
        readPtr,
        readLength);
}

constexpr std::array bindings{
    makeBinding<
        QuickJSV1ImportId::etxn_reserve,
        etxnReserveHandler,
        scalarMeasure>(),
    makeBinding<QuickJSV1ImportId::prepare, prepareHandler, prepareMeasure>(),
    makeBinding<QuickJSV1ImportId::emit, emitHandler, prepareMeasure>()};

}  // namespace

std::span<QuickJSImportBinding const>
emissionBindings() noexcept
{
    return bindings;
}

}  // namespace hook::quickjs
