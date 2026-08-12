#include <xrpld/app/hook/HookHostOperations/HookContext.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <array>

namespace hook::quickjs {
namespace {

raw::Result
hookAccountHandler(
    QuickJSHostCall& call,
    std::uint32_t writePtr,
    std::uint32_t writeLength)
{
    auto memory = call.memory();
    if (!memory)
        return hook_api::hook_return_code::INTERNAL_ERROR;
    return raw::hookAccount(call.hookContext(), *memory, writePtr, writeLength);
}

std::uint64_t
writeMeasure(std::uint32_t, std::uint32_t writeLength) noexcept
{
    return writeLength;
}

constexpr std::array bindings{makeBinding<
    QuickJSImportId::hook_account,
    hookAccountHandler,
    writeMeasure>()};

}  // namespace

std::span<QuickJSImportBinding const>
hookContextBindings() noexcept
{
    return bindings;
}

}  // namespace hook::quickjs
