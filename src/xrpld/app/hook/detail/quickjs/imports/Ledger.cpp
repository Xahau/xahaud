#include <xrpld/app/hook/HookHostOperations/Ledger.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <array>

namespace hook::quickjs {
namespace {

raw::Result
ledgerSequenceHandler(QuickJSHostCall& call)
{
    return raw::v1::ledgerSequence(call.hookContext());
}

raw::Result
ledgerLastTimeHandler(QuickJSHostCall& call)
{
    return raw::v1::ledgerLastTime(call.hookContext());
}

raw::Result
ledgerLastHashHandler(
    QuickJSHostCall& call,
    std::uint32_t writePtr,
    std::uint32_t writeLength)
{
    auto memory = call.memory();
    if (!memory)
        return hook_api::hook_return_code::INTERNAL_ERROR;
    return raw::v1::ledgerLastHash(
        call.hookContext(), *memory, writePtr, writeLength);
}

std::uint64_t
scalarMeasure() noexcept
{
    return 0;
}

std::uint64_t
writeMeasure(std::uint32_t, std::uint32_t writeLength) noexcept
{
    return writeLength;
}

constexpr std::array bindings{
    makeBinding<
        QuickJSV1ImportId::ledger_seq,
        ledgerSequenceHandler,
        scalarMeasure>(),
    makeBinding<
        QuickJSV1ImportId::ledger_last_time,
        ledgerLastTimeHandler,
        scalarMeasure>(),
    makeBinding<
        QuickJSV1ImportId::ledger_last_hash,
        ledgerLastHashHandler,
        writeMeasure>()};

}  // namespace

std::span<QuickJSImportBinding const>
ledgerBindings() noexcept
{
    return bindings;
}

}  // namespace hook::quickjs
