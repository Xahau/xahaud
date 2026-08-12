#include <xrpld/app/hook/HookHostOperations/OriginatingTransaction.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <array>

namespace hook::quickjs {
namespace {

raw::Result
otxnTypeHandler(QuickJSHostCall& call)
{
    return raw::otxnType(call.hookContext());
}

std::uint64_t
scalarMeasure() noexcept
{
    return 0;
}

constexpr std::array bindings{
    makeBinding<QuickJSImportId::otxn_type, otxnTypeHandler, scalarMeasure>()};

}  // namespace

std::span<QuickJSImportBinding const>
originatingTransactionBindings() noexcept
{
    return bindings;
}

}  // namespace hook::quickjs
