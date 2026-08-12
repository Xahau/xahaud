#include <xrpld/app/hook/HookHostOperations/OriginatingTransaction.h>
#include <xrpld/app/hook/applyHook.h>

namespace hook::raw {

Result
otxnType(HookContext& hookCtx)
{
    return hookCtx.api().otxn_type();
}

}  // namespace hook::raw
