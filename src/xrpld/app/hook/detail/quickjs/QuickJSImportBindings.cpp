#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>

namespace hook::quickjs {
namespace {

QuickJSImportBinding const*
findIn(
    std::span<QuickJSImportBinding const> bindings,
    QuickJSImportId id) noexcept
{
    for (auto const& binding : bindings)
        if (binding.id == id)
            return &binding;
    return nullptr;
}

}  // namespace

QuickJSImportBinding const*
findBinding(QuickJSImportId id, ImportCategory category) noexcept
{
    switch (category)
    {
        case ImportCategory::control:
            return findIn(controlBindings(), id);
        case ImportCategory::emission:
            return findIn(emissionBindings(), id);
        case ImportCategory::hookContext:
            return findIn(hookContextBindings(), id);
        case ImportCategory::ledger:
            return findIn(ledgerBindings(), id);
        case ImportCategory::originatingTransaction:
            return findIn(originatingTransactionBindings(), id);
        case ImportCategory::state:
            return findIn(stateBindings(), id);
        case ImportCategory::trace:
            return findIn(traceBindings(), id);
        case ImportCategory::util:
        case ImportCategory::serializedObject:
        case ImportCategory::floatingPoint:
        case ImportCategory::slot:
        case ImportCategory::unknown:
            return nullptr;
    }
    return nullptr;
}

}  // namespace hook::quickjs
